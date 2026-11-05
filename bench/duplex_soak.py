#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Retain a resident Input.Stream/io_step soak and quiescent resource observations."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import platform
import selectors
import signal
import subprocess
import sys
import time

from stream_soak import controller, digest, resources

ROOT = Path(__file__).resolve().parent.parent


class Session:
    """Private unreaped leader; bounded collection also applies during cleanup."""
    def __init__(self, binary, output_cap=16777216, error_cap=65536):
        self.proc = subprocess.Popen([str(binary)], cwd=ROOT, stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        self.selector = selectors.DefaultSelector()
        self.raw = bytearray(); self.errors = bytearray(); self.pending = bytearray()
        self.output_cap = output_cap; self.error_cap = error_cap
        self.overflow = False
        for stream, label in [(self.proc.stdout, 'out'), (self.proc.stderr, 'err')]:
            os.set_blocking(stream.fileno(), False)
            self.selector.register(stream, selectors.EVENT_READ, label)

    def read(self, key, protocol):
        data = os.read(key.fd, 65536)
        if not data:
            self.selector.unregister(key.fileobj)
            return False
        target, cap = (self.raw, self.output_cap) if key.data == 'out' else (self.errors, self.error_cap)
        remaining = max(0, cap - len(target))
        target.extend(data[:remaining])
        if len(data) > remaining:
            self.overflow = True
        if protocol and key.data == 'out':
            self.pending.extend(data[:max(0, 4097 - len(self.pending))])
            if len(self.pending) > 4096:
                raise RuntimeError('resident protocol line exceeded cap')
        return True

    def line(self, timeout=40):
        deadline = time.monotonic() + timeout
        while True:
            if self.overflow:
                raise RuntimeError('resident diagnostic retention cap exceeded')
            if b'\n' in self.pending:
                line, rest = self.pending.split(b'\n', 1)
                if rest:
                    raise RuntimeError('unsolicited resident protocol output')
                self.pending.clear()
                return line.decode('ascii')
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('resident protocol timed out')
            for key, _ in self.selector.select(min(1, remaining)):
                if not self.read(key, True) and key.data == 'out':
                    raise RuntimeError('resident exited before completing protocol')

    def send(self, text):
        self.proc.stdin.write(text.encode('ascii') + b'\n')
        self.proc.stdin.flush()

    def finish(self, timeout=10):
        # Never call communicate(): it allocates before checking a retention cap.
        try:
            self.proc.stdin.close()
        except BrokenPipeError:
            pass
        deadline = time.monotonic() + timeout
        while self.selector.get_map():
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('resident trailing output did not close')
            for key, _ in self.selector.select(min(1, remaining)):
                self.read(key, False)
        self.proc.wait(timeout=max(.001, deadline - time.monotonic()))
        if self.overflow:
            raise RuntimeError('resident diagnostic retention cap exceeded')

    def close(self):
        try:
            if self.proc.returncode is None:
                # No poll/wait or external waiter can release this private PID.
                try:
                    os.killpg(self.proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                try:
                    self.finish()
                finally:
                    # Even retention/protocol failure cannot bypass leader reap.
                    self.proc.wait(timeout=10)
        finally:
            self.selector.close()
            for stream in [self.proc.stdin, self.proc.stdout, self.proc.stderr]:
                stream.close()


def measure(args):
    initial = controller.git('status', '--porcelain')
    if initial and not args.smoke:
        raise ValueError('commit source before measurement')
    goose, cc = controller.executable(args.goose), controller.executable(args.cc)
    directory = ROOT / '.build/duplex-soak'; directory.mkdir(parents=True, exist_ok=True)
    binary, helper, worker = [directory / name for name in ['driver', 'helper', 'worker']]
    for source, output in [(ROOT / 'tests/helper.c', helper), (ROOT / 'examples/worker.c', worker)]:
        controller.command([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', source, '-o', output])
    controller.command([goose, 'build', ROOT / 'bench/duplex_soak.goose', '--cc', cc, '-O2', '-o', binary])
    if controller.adopted_cleanup():
        raise RuntimeError('preparation retained a child')
    inputs = dict(driver=binary, helper=helper, worker=worker, goose=goose, cc=cc,
                  controller=Path(__file__), source=ROOT / 'bench/duplex_soak.goose',
                  helper_source=ROOT / 'tests/helper.c', worker_source=ROOT / 'examples/worker.c',
                  package=ROOT / 'process.build', library=ROOT / 'process.goose',
                  native_process=ROOT / 'native/process.c', native_launch=ROOT / 'native/launch.c',
                  native_header=ROOT / 'native/process.h', native_internal=ROOT / 'native/internal.h',
                  resource_controller=ROOT / 'bench/stream_soak.py',
                  ownership_controller=ROOT / 'scripts/checker_consumer.py', cat=controller.executable('/bin/cat'))
    identities = {name: digest(path) for name, path in inputs.items()}
    report = dict(status='incomplete', source_revision=controller.git('rev-parse', 'HEAD'),
                  source_dirty=bool(initial), smoke=args.smoke, hashes=identities,
                  timestamp=datetime.now(timezone.utc).isoformat(), requested_seconds=args.seconds,
                  warmup_cases=12, seed=20261105, platform=platform.platform(),
                  cpu_affinity=sorted(os.sched_getaffinity(0)), load_start=list(os.getloadavg()),
                  cpu_model=next((line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines()
                                  if line.startswith('model name')), platform.machine()),
                  compiler_revision=(ROOT / 'GOOSE_REVISION').read_text().strip(),
                  goose_version=subprocess.check_output([goose, 'version'], text=True, timeout=30).strip(),
                  cc_version=subprocess.check_output([cc, '--version'], text=True, timeout=30).splitlines()[0],
                  build_flags='C -std=c11 -Wall -Wextra -Werror -O2; Goose --cc selected -O2',
                  columns=['index', 'kind', 'wall_ns', 'io_steps', 'input_accepted', 'stdout_bytes', 'stderr_bytes',
                           'exit_code', 'signal', 'error', 'empty_offers', 'nonempty_zero_accepts'],
                  kinds=['400 framed worker requests', '2MiB duplex burst slow feed/drain', 'early peer close TERM',
                         'active incremental cat TERM', 'open-input deadline', 'mixed capture overflow'],
                  samples=[], scope='Shared-host resident Input.Stream consumer, no exclusive isolation. '
                  'Timers include public calls, byte oracles and deliberate pauses; controller handshakes/probes excluded. '
                  'Quiescent resident /proc observations omit active peaks and child memory; no universal leak/stability proof. '
                  'Kernel acceptance differs from consumption; early close/cancel may discard accepted bytes. '
                  'No cross-language timing or safety ranking. Finish current six-case cycle after requested duration.')
    controller_before = len(list(Path('/proc/self/fd').iterdir()))
    session = Session(binary)
    try:
        if session.line() != 'ready' or os.getsid(session.proc.pid) != session.proc.pid:
            raise RuntimeError('invalid readiness/private session')
        index = 0; began = None
        while True:
            if index == 12:
                report['baseline'] = resources(session.proc.pid)
                if report['baseline']['children']:
                    raise RuntimeError('warmup retained a child')
                began = time.monotonic_ns()
            if began is not None and index % 6 == 0 and time.monotonic_ns() - began >= args.seconds * 1000000000:
                break
            session.send('next')
            line = session.line(); fields = line.split()
            sample = dict(phase='warmup' if index < 12 else 'measured', line=line)
            report['samples'].append(sample)
            if len(fields) != 13 or fields[0] != 'case':
                raise RuntimeError('malformed resident record')
            values = [int(value) for value in fields[1:]]; sample['record'] = values
            kind = index % 6
            if values[0] != index or values[1] != kind or values[2] <= 0 or values[3] <= 0 or any(n < 0 for n in values[4:7]):
                raise RuntimeError('resident record sequence/count differs')
            if values[9] != (4 if kind == 4 else 5 if kind == 5 else 0):
                raise RuntimeError('resident outcome differs')
            snapshot = resources(session.proc.pid); sample['resources'] = snapshot
            if snapshot['children'] or (began is not None and snapshot['fds'] != report['baseline']['fds']):
                raise RuntimeError('resident retained child or descriptors')
            sample['passed'] = True; index += 1
            if index % 60 == 0:
                print('Completed', max(0, index - 12), 'measured duplex cases', flush=True)
        report['measured_elapsed_ns'] = time.monotonic_ns() - began
        report['final_resources'] = resources(session.proc.pid)
        session.send('stop')
        if session.line() != 'stopped ' + str(index):
            raise RuntimeError('stop acknowledgment differs')
        recorded = bytes(session.raw)
        session.finish()
        if bytes(session.raw) != recorded or session.errors or session.proc.returncode != 0:
            raise RuntimeError('unexpected trailing output/status')
        if identities != {name: digest(path) for name, path in inputs.items()}:
            raise RuntimeError('measurement identity changed')
        if controller.git('rev-parse', 'HEAD') != report['source_revision'] or controller.git('status', '--porcelain') != initial:
            raise RuntimeError('measurement source changed')
        report['status'] = 'complete'
    except BaseException as error:
        report.update(status='failed', error_kind=type(error).__name__, error=str(error))
        raise
    finally:
        try:
            try:
                session.close()
            finally:
                report['adopted_cleanup_count'] = controller.adopted_cleanup()
            if report['adopted_cleanup_count']:
                raise RuntimeError('controller found an adopted fixture')
            if len(list(Path('/proc/self/fd').iterdir())) != controller_before:
                raise RuntimeError('controller retained descriptors')
        except BaseException as error:
            report.update(status='failed', cleanup_error_kind=type(error).__name__)
            raise
        finally:
            report.update(finished_at=datetime.now(timezone.utc).isoformat(), load_finish=list(os.getloadavg()),
                          stderr=session.errors.decode('utf-8', errors='replace'),
                          raw_stdout=session.raw.decode('ascii', errors='replace'),
                          controller_fds=[controller_before, len(list(Path('/proc/self/fd').iterdir()))])
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, separators=(',', ':')) + '\n')
            print('Retained report:', args.output, flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--goose', required=True); parser.add_argument('--cc', default='gcc')
    parser.add_argument('--seconds', type=int, default=300)
    parser.add_argument('--smoke', action='store_true', help='one-second functional check, permits declared dirty source')
    parser.add_argument('--output', type=Path, default=ROOT / '.build/duplex-soak.json')
    args = parser.parse_args()
    if not 1 <= args.seconds <= 3600 or (args.smoke and args.seconds != 1):
        parser.error('seconds must be1..3600; smoke requires1')
    controller.controller_scope()
    try:
        measure(args)
    finally:
        controller.adopted_cleanup()


if __name__ == '__main__':
    main()
