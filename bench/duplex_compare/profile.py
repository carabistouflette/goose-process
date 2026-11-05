#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Profile Git-matched successful Goose/Rust sessions separately from timing."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import subprocess

import measure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--measurement', type=Path, default=measure.ROOT / '.build/duplex-compare-2026-11-05.json')
    parser.add_argument('--output', type=Path, default=measure.ROOT / '.build/duplex-profile.json')
    parser.add_argument('--directory', type=Path, default=measure.ROOT / '.build/duplex-compare')
    parser.add_argument('--language', action='append', choices=['goose', 'rust'])
    parser.add_argument('--tool', action='append', choices=['strace', 'memcheck', 'callgrind'])
    args = parser.parse_args()
    recorded = json.loads(args.measurement.read_text())
    if recorded['status'] != 'complete' or measure.controller.git('status', '--porcelain'):
        raise ValueError('completed measurement and clean source required')
    measure.controller.controller_scope()
    directory = args.directory.resolve()
    worker = directory / 'worker'
    strace, valgrind = [measure.controller.executable(name) for name in ['strace', 'valgrind']]
    inputs = dict(worker=worker, binary_goose=directory / 'goose', binary_rust=directory / 'rust')
    hashes = {name: measure.digest(path) for name, path in inputs.items()}
    if any(value != recorded['hashes'][name] for name, value in hashes.items()):
        raise ValueError('profile artifacts differ from measured binaries')
    report = dict(status='incomplete', source_revision=measure.controller.git('rev-parse', 'HEAD'),
                  timestamp=datetime.now(timezone.utc).isoformat(), measurement_sha256=measure.digest(args.measurement),
                  measurement_scope=recorded['scope'], native_object_pairs=recorded.get('native_object_pairs'),
                  hashes=hashes, script_sha256=measure.digest(Path(__file__)),
                  tools={name: dict(sha256=measure.digest(path), version=measure.version(path, '--version').splitlines()[0])
                         for name, path in [('strace', strace), ('valgrind', valgrind)]},
                  scope='Separate parent-only successful fixed-session profiles, not untraced timing results. '
                  'Strace excludes child executable work. Memcheck/Callgrind include driver startup, runtime, '
                  'byte validation and adapter work, not isolated library costs. '
                  'No cancellation path or complete mmap-backed storage accounting. '
                  'Only checkout prefixes are normalized; raw log hashes retained. No general safety/performance ranking.',
                  runs=[])
    try:
        for language in args.language or ['goose', 'rust']:
            for size in [31, 4096]:
                for tool in ['strace', 'memcheck', 'callgrind']:
                    if args.tool and tool not in args.tool:
                        continue
                    if tool == 'callgrind' and size == 31:
                        continue
                    log = directory / f'{language}-{size}-{tool}.log'
                    if tool == 'strace':
                        command = [strace, '-c', '-o', log, '-e',
                                   'trace=read,write,poll,ppoll,wait4,sendto,close,pidfd_open,pidfd_send_signal']
                    elif tool == 'memcheck':
                        command = [valgrind, '--tool=memcheck', '--error-exitcode=99', '--track-origins=yes',
                                   '--leak-check=full', '--show-leak-kinds=all',
                                   '--errors-for-leak-kinds=definite,indirect,possible',
                                   '--child-silent-after-fork=yes', '--log-file=' + str(log)]
                    else:
                        command = [valgrind, '--tool=callgrind', '--callgrind-out-file=' + str(log),
                                   '--log-file=' + str(log) + '.errors']
                    sample = dict(language=language, payload_size=size, tool=tool, passed=False)
                    report['runs'].append(sample)
                    code, output, errors = measure.controller.run_owned(
                        [*command, directory / language, worker, str(size), '128'], timeout=180, stderr=subprocess.PIPE)
                    sample.update(returncode=code, stdout=output.decode('ascii', errors='replace'),
                                  stderr=errors.decode('utf-8', errors='replace').replace(str(measure.ROOT), '<source>'))
                    text = log.read_text(); sample['raw_log_sha256'] = measure.digest(log)
                    sample['log'] = text.replace(str(measure.ROOT), '<source>')
                    sample['normalized_log_sha256'] = hashlib.sha256(sample['log'].encode()).hexdigest()
                    if tool == 'callgrind':
                        sample['diagnostics'] = Path(str(log) + '.errors').read_text().replace(str(measure.ROOT), '<source>')
                        sample['annotation'] = subprocess.check_output(
                            ['callgrind_annotate', '--threshold=95', str(log)], text=True, timeout=30).replace(str(measure.ROOT), '<source>')
                    fields = output.split()
                    if code or len(fields) != 3 or [int(v) for v in fields[1:]] != [128 * (size + 4), 512]:
                        raise RuntimeError('instrumented session failed its byte/status oracle')
                    if measure.controller.adopted_cleanup():
                        raise RuntimeError('profile retained an adopted child')
                    sample['passed'] = True
        if hashes != {name: measure.digest(path) for name, path in inputs.items()}:
            raise RuntimeError('profile artifacts changed')
        if measure.controller.git('status', '--porcelain') or measure.controller.git('rev-parse', 'HEAD') != report['source_revision']:
            raise RuntimeError('profile source changed')
        report['status'] = 'complete'
    except BaseException as error:
        report.update(status='failed', error_kind=type(error).__name__, error=str(error))
        raise
    finally:
        try:
            if measure.controller.adopted_cleanup():
                report['status'] = 'failed'
                raise RuntimeError('final profile cleanup retained a child')
        finally:
            report['finished_at'] = datetime.now(timezone.utc).isoformat()
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2) + '\n')
            print('Retained profiles:', args.output)


if __name__ == '__main__':
    main()
