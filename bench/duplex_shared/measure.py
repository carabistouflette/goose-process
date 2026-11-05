#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Compare public Goose and diagnostic Rust wrappers around identical C objects."""
import argparse
from datetime import datetime, timezone
import importlib.util
import json
import os
from pathlib import Path
import platform
import random
import shlex
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('duplex_comparison', ROOT / 'bench/duplex_compare/measure.py')
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)
controller = base.controller
digest = base.digest


def sources():
    paths = [*sorted(HERE.glob('*.py')), HERE / 'driver.rs', HERE / 'README.md',
             ROOT / 'bench/duplex_compare/driver.goose', ROOT / 'bench/duplex_compare/measure.py',
             ROOT / 'bench/duplex_compare/fault.c', ROOT / 'examples/worker.c',
             ROOT / 'process.goose', ROOT / 'process.build', ROOT / 'GOOSE_REVISION',
             ROOT / 'scripts/native-manifest.py', ROOT / 'scripts/checker_consumer.py',
             *sorted((ROOT / 'native').glob('*.[ch]'))]
    return {str(path.relative_to(ROOT)): path for path in paths}


def build(args):
    tools = {name: controller.executable(value) for name, value in
             dict(goose=args.goose, cc=args.cc, ar='ar', python=sys.executable).items()}
    proxy = shutil.which('rustc')
    if proxy is None:
        raise ValueError('installed rustc required')
    sysroot = subprocess.check_output([proxy, '--print', 'sysroot'], text=True, timeout=30,
                                     env=dict(os.environ, RUSTUP_AUTO_INSTALL='0')).strip()
    tools['rustc'] = controller.executable(Path(sysroot) / 'bin/rustc')
    directory = args.directory.resolve(); directory.mkdir(parents=True, exist_ok=True)
    worker = directory / 'worker'
    binaries = dict(goose=directory / 'goose', rust=directory / 'rust')
    controller.command([tools['python'], ROOT / 'scripts/native-manifest.py', '--check'])
    controller.command([tools['cc'], '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
                        ROOT / 'examples/worker.c', '-o', worker])
    goose_command = [tools['goose'], 'build', ROOT / 'bench/duplex_compare/driver.goose',
                     '--cc', tools['cc'], '-O2', '-o', binaries['goose']]
    controller.command(goose_command)
    # Parse quoted source/object paths; never execute shell text from this plan.
    plan = subprocess.check_output([*goose_command, '--dry-run', '-v'], text=True, timeout=30)
    expected = {ROOT / 'native/process.c': 'process', ROOT / 'native/launch.c': 'launch'}
    native_objects = {}
    for line in plan.splitlines():
        arguments = shlex.split(line)
        if '-c' not in arguments or '-o' not in arguments:
            continue
        source = Path(arguments[arguments.index('-c') + 1]).resolve()
        if source in expected:
            native_objects[expected[source]] = Path(arguments[arguments.index('-o') + 1]).resolve()
    if set(native_objects) != {'process', 'launch'} or any(not path.is_file() for path in native_objects.values()):
        raise ValueError('cannot identify both native objects from pinned Goose build plan')
    copies = {}
    for name, path in native_objects.items():
        copies[name] = directory / (name + '.o')
        shutil.copy2(path, copies[name])
        if digest(path) != digest(copies[name]):
            raise RuntimeError('native object copy differs')
    archive = directory / 'libduplexshared.a'; archive.unlink(missing_ok=True)
    controller.command([tools['ar'], 'rcs', archive, *copies.values()])
    controller.command([tools['rustc'], '-C', 'opt-level=3', '-D', 'warnings', '--edition', '2021',
                        HERE / 'driver.rs', '-L', 'native=' + str(directory), '-l', 'static=duplexshared',
                        '-o', binaries['rust']])
    identities = {**{f'tool_{name}': path for name, path in tools.items()},
                  **{f'binary_{name}': path for name, path in binaries.items()},
                  **{f'goose_native_{name}': path for name, path in native_objects.items()},
                  **{f'rust_native_{name}': path for name, path in copies.items()},
                  **{f'source_{name}': path for name, path in sources().items()},
                  'native_archive': archive, 'worker': worker}
    versions = {name: base.version(path, *(['version'] if name == 'goose' else ['--version'])).splitlines()[0]
                for name, path in tools.items()}
    return worker, {name: [path] for name, path in binaries.items()}, identities, versions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--goose', default=str(ROOT / '.toolchain/goose/build/goose'))
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--directory', type=Path, default=ROOT / '.build/duplex-shared')
    parser.add_argument('--build-only', action='store_true', help='check four workloads without a measured series')
    parser.add_argument('--rounds', type=int, default=9)
    parser.add_argument('--calls', type=int, default=5)
    parser.add_argument('--output', type=Path, default=ROOT / '.build/duplex-shared.json')
    args = parser.parse_args()
    if not 1 <= args.rounds <= 99 or not 1 <= args.calls <= 100:
        parser.error('rounds1..99 and calls1..100 required')
    initial_status = controller.git('status', '--porcelain')
    if initial_status and not args.build_only:
        parser.error('commit source before timing')
    source_hashes = {name: digest(path) for name, path in sources().items()}
    controller.controller_scope()
    report = dict(status='incomplete', source_revision=controller.git('rev-parse', 'HEAD'),
                  source_dirty=bool(initial_status), timestamp=datetime.now(timezone.utc).isoformat(),
                  seed=20261106, rounds=args.rounds, calls_per_cell=args.calls, samples=[],
                  compiler_revision=(ROOT / 'GOOSE_REVISION').read_text().strip(),
                  platform=platform.platform(), cpu_affinity=sorted(os.sched_getaffinity(0)),
                  load_start=list(os.getloadavg()),
                  cpu_model=next((line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines()
                                  if line.startswith('model name')), platform.machine()),
                  flags=dict(goose='--cc selected -O2; native -O2 -fPIC -std=c11 -Wall -Wextra -Werror',
                             rust='-C opt-level=3 -D warnings --edition2021; exact Goose native object copies',
                             worker='-std=c11 -Wall -Wextra -Werror -O2'),
                  workload=dict(requests=128, payload_sizes=[31, 4096], stdout_read_buffer=257,
                                stderr_read_buffer=3, stderr_bytes=512, deadline_ms=30000,
                                pattern='(index*17+index//251)%256; fixed frame reused'),
                  scope='Shared-native-kernel diagnostic, not Rust std::process or independent process correctness. '
                  'Both routes link byte-identical Goose-built native objects with the same socket/pidfd/deadline/FD/signal policy. '
                  'Fixed wrapper validation/ownership/allocation differs; this is not full public-API parity. '
                  'Driver wall includes native availability/path validation/staging, launch,128framed exchanges, byte checks, EOF, drain/reap and close. '
                  'Driver startup/imports, frame construction and compilation excluded; auxiliary outer wall/CPU include startup and worker. '
                  'Each invocation is a new process, not a resident runtime. Shared host, inherited affinity, no exclusive isolation. '
                  'No general language, memory, safety or speed ranking.')
    try:
        worker, commands, identities, versions = build(args)
        if source_hashes != {name: digest(path) for name, path in sources().items()}:
            raise RuntimeError('sources changed while building')
        if controller.adopted_cleanup():
            raise RuntimeError('build retained a fixture child')
        report['versions'] = versions
        report['hashes'] = {name: digest(path) for name, path in identities.items()}
        report['native_object_pairs'] = {name: dict(goose=report['hashes']['goose_native_' + name],
                                                   rust=report['hashes']['rust_native_' + name])
                                          for name in ['process', 'launch']}
        before_fds = len(list(Path('/proc/self/fd').iterdir()))
        cells = [(language, size) for language in commands for size in [31, 4096]]
        if args.build_only:
            for language, size in cells:
                base.checked(commands[language], worker, size)
            print('4 same-native-object duplex workloads passed')
            return
        rng = random.Random(report['seed'])
        for round_number in range(args.rounds):
            order = cells.copy(); rng.shuffle(order)
            for order_index, (language, size) in enumerate(order):
                sample = dict(round=round_number, order=order_index, language=language,
                              payload_size=size, warmup=None, calls=[], passed=False)
                report['samples'].append(sample)
                report['active_invocation'] = dict(round=round_number, order=order_index, language=language,
                                                   payload_size=size, phase='warmup', call_index=0)
                sample['warmup'] = base.checked(commands[language], worker, size)
                for call_index in range(args.calls):
                    report['active_invocation'].update(phase='measured', call_index=call_index)
                    sample['calls'].append(base.checked(commands[language], worker, size))
                sample['passed'] = True
        report.pop('active_invocation', None)
        if report['hashes'] != {name: digest(path) for name, path in identities.items()}:
            raise RuntimeError('source/compiler/object/binary identity changed')
        if controller.git('rev-parse', 'HEAD') != report['source_revision'] or controller.git('status', '--porcelain') != initial_status:
            raise RuntimeError('source revision/worktree changed')
        report['controller_fds'] = [before_fds, len(list(Path('/proc/self/fd').iterdir()))]
        if report['controller_fds'] != [before_fds, before_fds]:
            raise RuntimeError('controller descriptor growth')
        report['status'] = 'complete'
    except BaseException as error:
        report.update(status='failed', error_kind=type(error).__name__, error=str(error))
        if isinstance(error, base.InvocationFailed):
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
                print('Retained report:', args.output)


if __name__ == '__main__':
    main()
