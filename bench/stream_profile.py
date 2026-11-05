#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Profile a short fixed protocol separately from the resident soak measurement."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
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
    args = parser.parse_args()
    controller.controller_scope()
    measured = json.loads(args.soak.read_text())
    binary, helper = ROOT / '.build/stream-soak/driver', ROOT / '.build/stream-soak/helper'
    if measured['status'] != 'complete' or controller.git('status', '--porcelain'):
        raise ValueError('completed soak and clean source required')
    if any(digest(path) != measured['hashes'][name] for name, path in [('driver', binary), ('helper', helper)]):
        raise ValueError('profile binaries differ from soak identities')
    strace, valgrind = controller.executable('strace'), controller.executable('valgrind')
    report = dict(status='incomplete', timestamp=datetime.now(timezone.utc).isoformat(),
                  source_revision=controller.git('rev-parse', 'HEAD'),
                  soak_sha256=digest(args.soak), driver_sha256=digest(binary), helper_sha256=digest(helper),
                  profile_script_sha256=digest(Path(__file__)),
                  versions={name: subprocess.check_output([path, '--version'], text=True, timeout=30).splitlines()[0]
                            for name, path in [('strace', strace), ('valgrind', valgrind)]},
                  scope='Two separate 12-case resident public-API profiles, not timing evidence. '
                  'Parent-only strace (no -f); child executable work excluded. '
                  'Memcheck sees native heap/access errors but does not fully account for Goose mmap-backed ownership/storage. '
                  'No absence-of-bugs, peak-memory, throughput or equal-policy comparison claim.', runs=[])
    configs = [
        ('strace', [strace, '-c', '-o', ROOT / '.build/stream-strace.txt', '-e',
                    'trace=read,poll,wait4,pipe2,socketpair,sendto,close,pidfd_open,pidfd_send_signal', binary]),
        ('valgrind', [valgrind, '--tool=memcheck', '--error-exitcode=99', '--track-origins=yes',
                      '--leak-check=full', '--show-leak-kinds=all',
                      '--errors-for-leak-kinds=definite,indirect,possible', '--child-silent-after-fork=yes',
                      '--log-file=' + str(ROOT / '.build/stream-valgrind.txt'), binary]),
    ]
    try:
        for name, command in configs:
            sample = dict(tool=name, passed=False)
            report['runs'].append(sample)
            code, output, errors = controller.run_owned(command, input_bytes=b'next\n' * 12 + b'stop\n',
                                                         timeout=180, stderr=subprocess.PIPE)
            sample.update(returncode=code, stdout=output.decode('ascii', errors='replace'),
                          stderr=errors.decode('utf-8', errors='replace'))
            log = ROOT / ('.build/stream-' + name + '.txt')
            sample['log'] = log.read_text()
            sample['log_sha256'] = digest(log)
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
