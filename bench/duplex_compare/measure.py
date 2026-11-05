#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Retain four bounded fixed-protocol implementations, not whole-API parity."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import resource
import shutil
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(ROOT / 'scripts'))
import checker_consumer as controller


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def version(binary, *arguments):
    return subprocess.check_output([binary, *arguments], timeout=30, text=True).strip()


class InvocationFailed(RuntimeError):
    def __init__(self, result):
        self.result = result
        super().__init__('driver invocation failed: ' + json.dumps(result))


def build(args):
    tools = {name: controller.executable(value) for name, value in
             dict(goose=args.goose, cc=args.cc, go='go', python=sys.executable).items()}
    # rustc may be a rustup multicall symlink: resolving that proxy changes argv0.
    # Query the installed toolchain, then fingerprint/invoke its actual compiler.
    rust_proxy = shutil.which('rustc')
    if rust_proxy is None:
        raise ValueError('rustc not found')
    sysroot = subprocess.check_output([rust_proxy, '--print', 'sysroot'], text=True, timeout=30,
                                     env=dict(os.environ, RUSTUP_AUTO_INSTALL='0')).strip()
    tools['rustc'] = controller.executable(Path(sysroot) / 'bin/rustc')
    directory = ROOT / '.build/duplex-compare'
    directory.mkdir(parents=True, exist_ok=True)
    worker = directory / 'worker'
    binaries = {name: directory / name for name in ('goose', 'rust', 'go')}
    controller.command([tools['cc'], '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                        ROOT / 'examples/worker.c', '-o', worker])
    controller.command([tools['goose'], 'build', HERE / 'driver.goose', '--cc', tools['cc'],
                        '-O2', '-o', binaries['goose']])
    controller.command([tools['rustc'], '-C', 'opt-level=3', '-D', 'warnings', '--edition', '2021',
                        HERE / 'driver.rs', '-o', binaries['rust']])
    environment = dict(os.environ, GOTOOLCHAIN='local', GOPROXY='off', GOSUMDB='off', GO111MODULE='off')
    controller.command([tools['go'], 'build', '-trimpath', '-o', binaries['go'], HERE / 'driver.go'], env=environment)
    commands = {name: [binary] for name, binary in binaries.items()}
    commands['python'] = [tools['python'], HERE / 'driver.py']
    identities = {**{f'tool_{name}': binary for name, binary in tools.items()},
                  **{f'binary_{name}': binary for name, binary in binaries.items()},
                  **{f'source_{name}': HERE / name for name in ('driver.goose', 'driver.rs', 'driver.go', 'driver.py', 'measure.py')},
                  **{f'library_{name}': ROOT / name for name in ('process.goose', 'process.build', 'native/process.c',
                                                              'native/launch.c', 'native/process.h', 'native/internal.h', 'GOOSE_REVISION')},
                  'worker': worker, 'worker_source': ROOT / 'examples/worker.c',
                  'ownership_controller': ROOT / 'scripts/checker_consumer.py'}
    versions = dict(goose=version(tools['goose'], 'version'), cc=version(tools['cc'], '--version').splitlines()[0],
                    rust=version(tools['rustc'], '--version'), go=version(tools['go'], 'version'),
                    python=version(tools['python'], '--version'))
    return worker, commands, identities, versions


def checked(command, worker, size, timeout=45):
    cpu_before = resource.getrusage(resource.RUSAGE_CHILDREN)
    began = time.monotonic_ns()
    watchdog = False
    try:
        code, output, errors = controller.run_owned([*command, worker, str(size), '128'],
                                                   timeout=timeout, stderr=subprocess.PIPE)
    except subprocess.TimeoutExpired as error:
        # run_owned kills/reaps its private group before propagating. Preserve
        # the bytes observed at timeout, rather than only str(TimeoutExpired).
        watchdog = True; code = None
        output, errors = error.output or b'', error.stderr or b''
    external_ns = time.monotonic_ns() - began
    cpu_after = resource.getrusage(resource.RUSAGE_CHILDREN)
    result = dict(returncode=code, watchdog_timeout=watchdog, stdout=output.decode('ascii', errors='replace'),
                  stderr=errors.decode('utf-8', errors='replace'), outer_wall_ns=external_ns,
                  process_tree_user_seconds=cpu_after.ru_utime - cpu_before.ru_utime,
                  process_tree_system_seconds=cpu_after.ru_stime - cpu_before.ru_stime, passed=False)
    result['adopted_cleanup_count'] = controller.adopted_cleanup()
    if watchdog or code or errors or result['adopted_cleanup_count']:
        raise InvocationFailed(result)
    fields = output.split()
    if len(fields) != 3:
        raise InvocationFailed(result)
    try:
        values = [int(value) for value in fields]
    except ValueError as error:
        raise InvocationFailed(result) from error
    if values[0] <= 0 or values[1:] != [128 * (size + 4), 512]:
        raise InvocationFailed(result)
    result.update(wall_ns=values[0], stdout_bytes=values[1], stderr_bytes=values[2], passed=True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--goose', default=str(ROOT / '.toolchain/goose/build/goose'))
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--build-only', action='store_true', help='build and check8 successful fixed workloads, no measured series')
    parser.add_argument('--rounds', type=int, default=9)
    parser.add_argument('--calls', type=int, default=5)
    parser.add_argument('--output', type=Path, default=ROOT / '.build/duplex-compare.json')
    args = parser.parse_args()
    if not 1 <= args.rounds <= 99 or not 1 <= args.calls <= 100:
        parser.error('rounds1..99 and calls1..100 required')
    initial_status = controller.git('status', '--porcelain')
    if initial_status and not args.build_only:
        parser.error('commit all sources before retaining measurements')
    controller.controller_scope()
    report = dict(status='incomplete', source_revision=controller.git('rev-parse', 'HEAD'),
                  source_dirty=bool(initial_status), timestamp=datetime.now(timezone.utc).isoformat(),
                  seed=20261105, rounds=args.rounds, calls_per_cell=args.calls, samples=[],
                  platform=platform.platform(), cpu_affinity=sorted(os.sched_getaffinity(0)),
                  load_start=list(os.getloadavg()), compiler_revision=(ROOT / 'GOOSE_REVISION').read_text().strip(),
                  cpu_model=next((line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines()
                                  if line.startswith('model name')), platform.machine()),
                  workload=dict(requests=128, payload_sizes=[31, 4096], stdout_read_buffer=257, stderr_read_buffer=3,
                                stderr_bytes=512, deadline_ms=30000, frames='u32 big-endian length followed by fixed binary pattern',
                                pattern='(index*17+index//251)%256; same independently checked frame per request'),
                  flags=dict(worker='-std=c11 -Wall -Wextra -Werror -O2', goose='--cc selected -O2',
                             rust='-C opt-level=3 -D warnings --edition2021', go='build -trimpath; local/offline toolchain'),
                  scope='Shared host with inherited CPU affinity, no exclusive isolation. '
                  'Fixed successful protocol with per-request stdout/stderr bounds and30s deadlines in all adapters. '
                  'Reported driver wall time includes worker launch,128sequential exchanges, byte oracles, explicit EOF, drain/reap and local cleanup. '
                  'Driver startup/imports, fixed frame construction and compilation excluded. '
                  'Outer wall and process-tree CPU include driver startup/imports and worker; they are auxiliary, not API-only CPU. '
                  'Input socket vs pipes, poll/selector vs reader goroutines, allocation, signal/FD setup and exceptional cleanup differ. '
                  'Not whole-API/safety parity, memory measurement, raw spawn cost, per-request latency distribution or a general language ranking.')
    try:
        worker, commands, identities, versions = build(args)
        if controller.adopted_cleanup():
            raise RuntimeError('build retained a child')
        report['versions'] = versions
        report['hashes'] = {name: digest(path) for name, path in identities.items()}
        before_fds = len(list(Path('/proc/self/fd').iterdir()))
        cells = [(language, size) for language in commands for size in (31, 4096)]
        if args.build_only:
            for language, size in cells:
                checked(commands[language], worker, size)
            print('8 bounded duplex driver/worker workloads passed')
            return
        rng = random.Random(report['seed'])
        for round_number in range(args.rounds):
            order = cells.copy(); rng.shuffle(order)
            for order_index, (language, size) in enumerate(order):
                sample = dict(round=round_number, order=order_index, language=language, payload_size=size,
                              warmup=None, calls=[], passed=False)
                report['samples'].append(sample)
                report['active_invocation'] = dict(round=round_number, order=order_index, language=language,
                                                   payload_size=size, phase='warmup', call_index=0)
                sample['warmup'] = checked(commands[language], worker, size)
                for call_index in range(args.calls):
                    report['active_invocation'].update(phase='measured', call_index=call_index)
                    sample['calls'].append(checked(commands[language], worker, size))
                sample['passed'] = True
        report.pop('active_invocation', None)
        if report['hashes'] != {name: digest(path) for name, path in identities.items()}:
            raise RuntimeError('source/compiler/binary identity changed')
        if controller.git('rev-parse', 'HEAD') != report['source_revision'] or controller.git('status', '--porcelain') != initial_status:
            raise RuntimeError('source revision/worktree changed')
        report['controller_fds'] = [before_fds, len(list(Path('/proc/self/fd').iterdir()))]
        if report['controller_fds'] != [before_fds, before_fds]:
            raise RuntimeError('controller descriptor growth')
        report['status'] = 'complete'
    except BaseException as error:
        report.update(status='failed', error_kind=type(error).__name__, error=str(error))
        if isinstance(error, InvocationFailed):
            report['failed_invocation'] = error.result
        raise
    finally:
        try:
            if controller.adopted_cleanup():
                report['status'] = 'failed'
                raise RuntimeError('final cleanup found an adopted child')
        finally:
            if not args.build_only:
                report.update(finished_at=datetime.now(timezone.utc).isoformat(), load_finish=list(os.getloadavg()))
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(json.dumps(report, indent=2) + '\n')
                print('Retained report:', args.output, flush=True)


if __name__ == '__main__':
    main()
