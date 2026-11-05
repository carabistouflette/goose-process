#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Interleave frozen before/after shared-engine binaries without rebuilding them."""
import argparse
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import random
import subprocess

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('shared_duplex', ROOT / 'bench/duplex_shared/measure.py')
shared = importlib.util.module_from_spec(spec); spec.loader.exec_module(shared)


def artifacts(directory, recorded):
    names = dict(binary_goose='goose', binary_rust='rust', worker='worker',
                 rust_native_process='process.o', rust_native_launch='launch.o', native_archive='libduplexshared.a')
    result = {name: directory / file for name, file in names.items()}
    for name, path in result.items():
        if shared.digest(path) != recorded['hashes'][name]:
            raise ValueError('frozen artifact differs: ' + name)
    for name in ['process', 'launch']:
        pair = recorded['native_object_pairs'][name]
        if pair['goose'] != pair['rust'] or pair['rust'] != recorded['hashes']['rust_native_' + name]:
            raise ValueError('native object policy differs within a phase')
    for name, expected in recorded['hashes'].items():
        if name.startswith('source_'):
            blob = subprocess.check_output(['git', 'show', recorded['source_revision'] + ':' + name[7:]],
                                           cwd=ROOT, timeout=30)
            if hashlib.sha256(blob).hexdigest() != expected:
                raise ValueError('recorded source differs from its Git object: ' + name)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--before', type=Path, required=True); parser.add_argument('--after', type=Path, required=True)
    parser.add_argument('--before-directory', type=Path, required=True)
    parser.add_argument('--after-directory', type=Path, required=True)
    parser.add_argument('--output', type=Path, default=ROOT / '.build/duplex-paired.json')
    args = parser.parse_args()
    if shared.controller.git('status', '--porcelain'):
        parser.error('commit sources before timing')
    records = {phase: json.loads(path.read_text()) for phase, path in [('before', args.before), ('after', args.after)]}
    if any(record['status'] != 'complete' for record in records.values()):
        raise ValueError('both shared-engine measurements must be complete')
    binaries = {phase: artifacts(directory.resolve(), records[phase]) for phase, directory in
                [('before', args.before_directory), ('after', args.after_directory)]}
    if shared.digest(binaries['before']['worker']) != shared.digest(binaries['after']['worker']):
        raise ValueError('worker binary changed')
    for name, value in records['before']['hashes'].items():
        if name.startswith('tool_') and records['after']['hashes'].get(name) != value:
            raise ValueError('compiler/tool changed between phases')
    inputs = dict(script=Path(__file__), before_record=args.before, after_record=args.after,
                  invocation_controller=ROOT / 'bench/duplex_compare/measure.py',
                  ownership_controller=ROOT / 'scripts/checker_consumer.py',
                  **{phase + '_' + name: path for phase, values in binaries.items() for name, path in values.items()})
    hashes = {name: shared.digest(path) for name, path in inputs.items()}
    shared.controller.controller_scope()
    fds = len(list(Path('/proc/self/fd').iterdir()))
    report = dict(status='incomplete', source_revision=shared.controller.git('rev-parse', 'HEAD'),
                  timestamp=datetime.now(timezone.utc).isoformat(), hashes=hashes,
                  phases={phase: dict(source_revision=r['source_revision'], hashes=r['hashes'],
                                      native_object_pairs=r['native_object_pairs'], versions=r['versions'])
                          for phase, r in records.items()},
                  seed=20261105, rounds=9, calls_per_cell=5, cpu_affinity=sorted(os.sched_getaffinity(0)),
                  platform=platform.platform(), load_start=list(os.getloadavg()), samples=[],
                  scope='Frozen before/after executables, exact shared C objects within each phase. '
                  'Both phases use one byte-identical worker. Nine shuffled rounds; one warmup and five sessions per cell. '
                  'Same128framed requests/31or4096payload/stdout257/stderr3/30s native budget. '
                  'Whole checked-session wall times exclude driver startup/frame construction; auxiliary CPU includes startup/worker. '
                  'Shared host, no exclusive isolation, no general speed/memory/safety ranking or independent kernel correctness proof.')
    try:
        cells = [(phase, language, size) for phase in ['before', 'after'] for language in ['goose', 'rust'] for size in [31, 4096]]
        rng = random.Random(report['seed'])
        for round_number in range(9):
            order = cells.copy(); rng.shuffle(order)
            for order_index, (phase, language, size) in enumerate(order):
                sample = dict(round=round_number, order=order_index, phase=phase, language=language,
                              payload_size=size, warmup=None, calls=[], passed=False)
                report['samples'].append(sample)
                report['active_invocation'] = dict(round=round_number, order=order_index, phase=phase,
                                                   language=language, payload_size=size, kind='warmup', call_index=0)
                command, worker = [binaries[phase]['binary_' + language]], binaries['before']['worker']
                sample['warmup'] = shared.base.checked(command, worker, size)
                for call_index in range(5):
                    report['active_invocation'].update(kind='measured', call_index=call_index)
                    sample['calls'].append(shared.base.checked(command, worker, size))
                sample['passed'] = True
        report.pop('active_invocation', None)
        if hashes != {name: shared.digest(path) for name, path in inputs.items()}:
            raise RuntimeError('paired artifact/script/record changed')
        if shared.controller.git('status', '--porcelain') or shared.controller.git('rev-parse', 'HEAD') != report['source_revision']:
            raise RuntimeError('paired source changed')
        report['controller_fds'] = [fds, len(list(Path('/proc/self/fd').iterdir()))]
        if report['controller_fds'] != [fds, fds]:
            raise RuntimeError('paired controller descriptor growth')
        report['status'] = 'complete'
    except BaseException as error:
        report.update(status='failed', error_kind=type(error).__name__, error=str(error))
        if isinstance(error, shared.base.InvocationFailed): report['failed_invocation'] = error.result
        raise
    finally:
        try:
            if shared.controller.adopted_cleanup():
                report['status'] = 'failed'; raise RuntimeError('paired controller adopted a fixture')
        finally:
            report.update(finished_at=datetime.now(timezone.utc).isoformat(), load_finish=list(os.getloadavg()))
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2) + '\n')
            print('Retained paired report:', args.output)


if __name__ == '__main__':
    main()
