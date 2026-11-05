#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Supervise one resident public-API streaming consumer for a fixed duration."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import selectors
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import checker_consumer as controller


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def resources(pid):
    root = Path('/proc') / str(pid)
    status = dict(line.split(':', 1) for line in (root / 'status').read_text().splitlines() if ':' in line)
    return dict(fds=len(list((root / 'fd').iterdir())),
                children=[int(n) for n in (root / 'task' / str(pid) / 'children').read_text().split()],
                rss_kib=int(status['VmRSS'].split()[0]), vmsize_kib=int(status['VmSize'].split()[0]),
                maps=len((root / 'maps').read_text().splitlines()))


class Session:
    def __init__(self, binary):
        self.proc = subprocess.Popen([str(binary)], cwd=ROOT, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        self.selector = selectors.DefaultSelector()
        self.pending = bytearray(); self.raw = bytearray(); self.errors = bytearray()
        for stream, label in [(self.proc.stdout, 'out'), (self.proc.stderr, 'err')]:
            os.set_blocking(stream.fileno(), False)
            self.selector.register(stream, selectors.EVENT_READ, label)

    def line(self, timeout=40):
        deadline = time.monotonic() + timeout
        while True:
            if b'\n' in self.pending:
                line, rest = self.pending.split(b'\n', 1)
                if rest:
                    raise RuntimeError('unsolicited protocol output')
                self.pending.clear()
                return line.decode('ascii')
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('resident consumer protocol timed out')
            for key, _ in self.selector.select(min(1, remaining)):
                data = os.read(key.fd, 65536)
                if not data:
                    self.selector.unregister(key.fileobj)
                    if key.data == 'out':
                        raise RuntimeError('resident consumer exited before completing protocol')
                    continue
                target = self.errors if key.data == 'err' else self.raw
                target.extend(data)
                if len(target) > (1048576 if key.data == 'err' else 8388608):
                    raise RuntimeError('consumer protocol output exceeded controller cap')
                if key.data == 'out':
                    self.pending.extend(data)
                    if len(self.pending) > 4096:
                        raise RuntimeError('consumer protocol line exceeded cap')

    def send(self, text):
        self.proc.stdin.write(text.encode('ascii') + b'\n')
        self.proc.stdin.flush()

    def close(self):
        self.selector.close()
        try:
            if self.proc.returncode is None:
                # No poll/wait/communicate has reaped this private leader yet.
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                self.proc.communicate(timeout=10)
        finally:
            for stream in [self.proc.stdin, self.proc.stdout, self.proc.stderr]:
                stream.close()


def measure(args):
    if controller.git('status', '--porcelain'):
        raise ValueError('commit source before measurement')
    goose, cc = controller.executable(args.goose), controller.executable(args.cc)
    directory = ROOT / '.build/stream-soak'; directory.mkdir(parents=True, exist_ok=True)
    binary, helper = directory / 'driver', directory / 'helper'
    controller.command([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', ROOT / 'tests/helper.c', '-o', helper])
    controller.command([goose, 'build', ROOT / 'bench/stream_soak.goose', '--cc', cc, '-O2', '-o', binary])
    if controller.adopted_cleanup():
        raise RuntimeError('preparation retained a child')
    inputs = dict(driver=binary, helper=helper, goose=goose, cc=cc,
                  controller=Path(__file__), source=ROOT / 'bench/stream_soak.goose',
                  helper_source=ROOT / 'tests/helper.c', package=ROOT / 'process.build',
                  ownership_controller=ROOT / 'scripts/checker_consumer.py',
                  cat=controller.executable('/bin/cat'), true=controller.executable('/bin/true'))
    identities = {name: digest(path) for name, path in inputs.items()}
    report = dict(status='incomplete', source_revision=controller.git('rev-parse', 'HEAD'),
                  hashes=identities, timestamp=datetime.now(timezone.utc).isoformat(),
                  requested_seconds=args.seconds, warmup_cases=12, platform=platform.platform(),
                  cpu_affinity=sorted(os.sched_getaffinity(0)), load_start=list(os.getloadavg()),
                  cpu_model=next((s.split(':', 1)[1].strip() for s in Path('/proc/cpuinfo').read_text().splitlines()
                                  if s.startswith('model name')), platform.machine()),
                  compiler_revision=(ROOT / 'GOOSE_REVISION').read_text().strip(),
                  goose_version=subprocess.check_output([goose, 'version'], text=True, timeout=30).strip(),
                  cc_version=subprocess.check_output([cc, '--version'], text=True, timeout=30).splitlines()[0],
                  build_flags='helper -std=c11 -Wall -Wextra -Werror -O2; Goose --cc selected -O2',
                  columns=['index', 'kind', 'wall_ns', 'stream_steps', 'stdout_bytes', 'stderr_bytes',
                           'exit_code', 'signal', 'prior_reaped', 'signal_code', 'signal_errno'],
                  kinds=['slow binary dual 3MiB', 'binary stdin1MiB', 'flood TERM', 'near-exit TERM',
                         'early input close', 'mixed stream/capture'],
                  samples=[], snapshots=[], scope='One shared-host resident public-API run, no exclusive isolation. '
                  'At least requested measured seconds after12warmup cases; finish current case. '
                  'Per-case latency includes byte validation and deliberate slow-reader pauses; controller probes/handshakes excluded. '
                  'Quiescent /proc snapshots belong to the resident Goose PID, not its spawner. '
                  'RSS/maps observations are not a universal no-leak proof or allocator accounting. '
                  'No language comparison, safety ranking or throughput speedup claim.')
    session = Session(binary)
    measured_start = None
    try:
        if session.line() != 'ready' or os.getsid(session.proc.pid) != session.proc.pid:
            raise RuntimeError('invalid consumer readiness/session')
        index = 0; baseline = None
        while True:
            if index == 12:
                baseline = resources(session.proc.pid)
                if baseline['children']:
                    raise RuntimeError('warmup retained a child')
                report['baseline'] = baseline
                measured_start = time.monotonic_ns()
            if measured_start is not None and time.monotonic_ns() - measured_start >= args.seconds * 1000000000:
                break
            session.send('next')
            line = session.line(); fields = line.split()
            sample = dict(phase='warmup' if index < 12 else 'measured', line=line)
            report['samples'].append(sample)
            if len(fields) != 12 or fields[0] != 'case':
                raise RuntimeError('malformed consumer record')
            values = [int(n) for n in fields[1:]]
            sample['record'] = values
            if values[0] != index or values[1] != index % 6 or values[2] <= 0 or values[3] <= 0:
                raise RuntimeError('consumer record sequence differs')
            snapshot = resources(session.proc.pid)
            sample['fds'] = snapshot['fds']; sample['children'] = snapshot['children']
            if snapshot['children'] or (baseline and snapshot['fds'] != baseline['fds']):
                raise RuntimeError('consumer retained descriptors or children')
            if index % 6 == 5:
                report['snapshots'].append(dict(index=index, phase=sample['phase'], **snapshot))
            sample['passed'] = True
            index += 1
            if measured_start is not None and index % 600 == 0:
                print('Completed', index - 12, 'measured cases', flush=True)
        report['measured_elapsed_ns'] = time.monotonic_ns() - measured_start
        report['final_resources'] = resources(session.proc.pid)
        session.send('stop')
        if session.line() != 'stopped ' + str(index):
            raise RuntimeError('consumer stop acknowledgment differs')
        # Protocol is complete; no future group signaling uses the leader PID.
        extra_out, extra_err = session.proc.communicate(timeout=10)
        if extra_out or extra_err or session.errors or session.proc.returncode != 0:
            raise RuntimeError('consumer reported unexpected output or status')
        if identities != {name: digest(path) for name, path in inputs.items()}:
            raise RuntimeError('measurement identities changed')
        if controller.git('rev-parse', 'HEAD') != report['source_revision'] or controller.git('status', '--porcelain'):
            raise RuntimeError('measurement source changed')
        report['status'] = 'complete'
    except BaseException as error:
        report.update(status='failed', error_kind=type(error).__name__)
        raise
    finally:
        try:
            session.close()
            if controller.adopted_cleanup():
                report['status'] = 'failed'
                raise RuntimeError('controller found an adopted fixture')
        except BaseException as error:
            report.update(status='failed', cleanup_error_kind=type(error).__name__)
            raise
        finally:
            report.update(finished_at=datetime.now(timezone.utc).isoformat(), load_finish=list(os.getloadavg()),
                          stderr=session.errors.decode('utf-8', errors='replace'),
                          raw_stdout=session.raw.decode('ascii', errors='replace'))
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, separators=(',', ':')) + '\n')
            print('Retained report:', args.output, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--goose', required=True)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--seconds', type=int, default=300)
    parser.add_argument('--output', type=Path, default=ROOT / '.build/stream-soak.json')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600:
        parser.error('seconds must be1..3600')
    controller.controller_scope()
    try:
        measure(args)
    finally:
        controller.adopted_cleanup()


if __name__ == '__main__':
    main()
