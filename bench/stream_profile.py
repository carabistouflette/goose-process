#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Profile a short fixed protocol separately from the resident soak measurement."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import shutil
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import checker_consumer as controller


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--soak', type=Path, default=ROOT / '.build/stream-soak.json')
    parser.add_argument('--output', type=Path, default=ROOT / '.build/stream-profile.json')
    parser.add_argument('--finite-only', action='store_true')
    parser.add_argument('--goose', default=str(ROOT / '.toolchain/goose/build/goose'))
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--tool', action='append', choices=['strace', 'valgrind', 'callgrind'])
    args = parser.parse_args()
    controller.controller_scope()
    measured = json.loads(args.soak.read_text())
    binary, helper = ROOT / '.build/stream-soak/driver', ROOT / '.build/stream-soak/helper'
    if measured['status'] != 'complete' or controller.git('status', '--porcelain'):
        raise ValueError('completed soak and clean source required')
    if any(digest(path) != measured['hashes'][name] for name, path in [('driver', binary), ('helper', helper)]):
        raise ValueError('profile binaries differ from soak identities')
    measured_binary_hash = digest(binary)
    if args.finite_only:
        # A separate profile executable preserves the measured resident binary.
        goose, cc = controller.executable(args.goose), controller.executable(args.cc)
        if digest(goose) != measured['hashes']['goose'] or digest(cc) != measured['hashes']['cc']:
            raise ValueError('profile compiler identities differ from soak')
        directory = ROOT / '.build/stream-soak-profile'; directory.mkdir(exist_ok=True)
        binary = directory / 'driver'
        shutil.copy2(helper, directory / 'helper')
        helper = directory / 'helper'
        controller.command([goose, 'build', ROOT / 'bench/stream_soak.goose', '--cc', cc, '-O2', '-o', binary])
        if controller.adopted_cleanup():
            raise RuntimeError('profile build retained a child')
    strace, valgrind = controller.executable('strace'), controller.executable('valgrind')
    report = dict(status='incomplete', timestamp=datetime.now(timezone.utc).isoformat(),
                  source_revision=controller.git('rev-parse', 'HEAD'),
                  soak_sha256=digest(args.soak), driver_sha256=digest(binary), helper_sha256=digest(helper),
                  measured_driver_sha256=measured_binary_hash, finite_only=args.finite_only,
                  driver_source_sha256=digest(ROOT / 'bench/stream_soak.goose'),
                  profile_script_sha256=digest(Path(__file__)),
                  versions={name: subprocess.check_output([path, '--version'], text=True, timeout=30).splitlines()[0]
                            for name, path in [('strace', strace), ('valgrind', valgrind)]},
                  scope='Separate 12-case resident public-API profiles, not timing evidence. '
                  'Parent-only strace (no -f); child executable work excluded. '
                  'Memcheck sees native heap/access errors but does not fully account for Goose mmap-backed ownership/storage. '
                  'Finite-only mode rebuilds a separate driver with only kinds0,1,4,5; it excludes cancellation/signal races. '
                  'Callgrind instruction counts include byte validation and harness code, not just the library. '
                  'No absence-of-bugs, peak-memory, throughput or equal-policy comparison claim. '
                  'Published text replaces only the checkout prefix with <source>; original local logs retain raw hashes.', runs=[])
    configs = [
        ('strace', [strace, '-c', '-o', ROOT / '.build/stream-strace.txt', '-e',
                    'trace=read,poll,wait4,pipe2,socketpair,sendto,close,pidfd_open,pidfd_send_signal', binary]),
        ('valgrind', [valgrind, '--tool=memcheck', '--error-exitcode=99', '--track-origins=yes',
                      '--leak-check=full', '--show-leak-kinds=all',
                      '--errors-for-leak-kinds=definite,indirect,possible', '--child-silent-after-fork=yes',
                      '--log-file=' + str(ROOT / '.build/stream-valgrind.txt'), binary]),
        ('callgrind', [valgrind, '--tool=callgrind', '--callgrind-out-file=' + str(ROOT / '.build/stream-callgrind.txt'),
                      '--log-file=' + str(ROOT / '.build/stream-callgrind-errors.txt'), binary]),
    ]
    try:
        for name, command in configs:
            if name not in (args.tool or ['strace', 'valgrind']):
                continue
            sample = dict(tool=name, passed=False)
            report['runs'].append(sample)
            protocol = b'finite\n' if args.finite_only else b'next\n'
            code, output, errors = controller.run_owned(command, input_bytes=protocol * 12 + b'stop\n',
                                                         timeout=180, stderr=subprocess.PIPE)
            sample.update(returncode=code, stdout=output.decode('ascii', errors='replace'),
                          stderr=errors.decode('utf-8', errors='replace').replace(str(ROOT), '<source>'))
            log = ROOT / ('.build/stream-' + name + '.txt')
            sample['log'] = log.read_text().replace(str(ROOT), '<source>')
            sample['raw_log_sha256'] = digest(log)
            sample['log_sha256'] = hashlib.sha256(sample['log'].encode()).hexdigest()
            if name == 'callgrind':
                annotation = subprocess.check_output(['callgrind_annotate', '--threshold=95', str(log)], text=True, timeout=30)
                sample['annotation'] = annotation.replace(str(ROOT), '<source>')
            lines = output.splitlines()
            if code or len(lines) != 14 or lines[0] != b'ready' or lines[-1] != b'stopped 12':
                raise RuntimeError('instrumented driver failed its fixed protocol')
            if controller.adopted_cleanup():
                raise RuntimeError('instrumented driver retained a child')
            sample['passed'] = True
        if digest(binary) != report['driver_sha256'] or digest(helper) != report['helper_sha256']:
            raise RuntimeError('profile identities changed')
        if controller.git('status', '--porcelain') or controller.git('rev-parse', 'HEAD') != report['source_revision']:
            raise RuntimeError('profile source changed')
        report['status'] = 'complete'
    except BaseException as error:
        report.update(status='failed', error_kind=type(error).__name__, error=str(error))
        raise
    finally:
        try:
            if controller.adopted_cleanup():
                report['status'] = 'failed'
                raise RuntimeError('profile cleanup found an adopted child')
        finally:
            report['finished_at'] = datetime.now(timezone.utc).isoformat()
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2) + '\n')
            print('Retained profiles:', args.output)


if __name__ == '__main__':
    main()
