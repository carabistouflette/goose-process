#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Retain a bounded pipeline convenience-copy/borrowed-capture comparison."""
import argparse
import base64
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import resource
import subprocess
import sys
import time
import traceback

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import checker_consumer as controller

SEED = 20261105
CELLS = [(route, case) for route in ('convenience', 'owner') for case in ('tiny', 'pressure')]


def timestamp():
    return datetime.now(timezone.utc).isoformat()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def raw(data):
    data = data or b''
    return dict(base64=base64.b64encode(data).decode('ascii'),
                text=data.decode('utf-8', errors='replace'), bytes=len(data))


def execute(arguments, record, timeout=45):
    """Retain partial watchdog output and recover every adopted fixture."""
    record.update(argv=list(map(str, arguments)), status='running', timeout_seconds=timeout)
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    began = time.monotonic_ns()
    try:
        code, out, err = controller.run_owned(arguments, timeout=timeout, stderr=subprocess.PIPE)
        record.update(returncode=code, stdout=raw(out), stderr=raw(err))
        if code:
            raise RuntimeError(f'command exited {code}')
        record['status'] = 'complete'
        return out
    except subprocess.TimeoutExpired as error:
        record.update(status='failed', watchdog=True, returncode=None,
                      stdout=raw(error.output), stderr=raw(error.stderr),
                      error_kind=type(error).__name__, error=str(error))
        raise
    except BaseException as error:
        record.update(status='failed', error_kind=type(error).__name__, error=str(error))
        raise
    finally:
        record['process_wall_ns_auxiliary'] = time.monotonic_ns() - began
        after = resource.getrusage(resource.RUSAGE_CHILDREN)
        record['startup_inclusive_cpu_seconds_auxiliary'] = dict(
            user=after.ru_utime - before.ru_utime, system=after.ru_stime - before.ru_stime)
        try:
            adopted = controller.adopted_cleanup()
            record['adopted_children_recovered'] = adopted
            if adopted:
                raise RuntimeError(f'command retained {adopted} adopted children; recovered')
        except BaseException as error:
            record.update(status='failed', cleanup_error_kind=type(error).__name__, cleanup_error=str(error))
            raise


def git_identity(directory):
    return dict(root=controller.git('rev-parse', '--show-toplevel', cwd=directory),
                revision=controller.git('rev-parse', 'HEAD', cwd=directory),
                status=controller.git('status', '--porcelain', '--untracked-files=normal', cwd=directory))


def native_fingerprint():
    result = hashlib.sha256()
    paths = [ROOT / 'scripts/native-manifest.py']
    paths += [path for path in (ROOT / 'native').rglob('*') if path.is_file()]
    for path in sorted(paths):
        name, data = path.relative_to(ROOT).as_posix().encode(), path.read_bytes()
        result.update(len(name).to_bytes(8, 'big') + name)
        result.update(len(data).to_bytes(8, 'big') + data)
    fingerprint = result.hexdigest()
    if f'// Source SHA256: {fingerprint}\n' not in (ROOT / 'process.build').read_text():
        raise ValueError('native source fingerprint is stale; run make format')
    return fingerprint


def snapshot(goose, cc, report, label, binaries=()):
    # Capture source/tool identities before invoking any build, then compare after.
    paths = [ROOT / name for name in (
        'process.goose', 'process.build', 'GOOSE_REVISION', 'bench/pipeline.goose',
        'bench/pipeline_measure.py', 'tests/pipeline_helper.c', 'scripts/checker_consumer.py',
        'scripts/native-manifest.py')]
    paths += [path for path in (ROOT / 'native').rglob('*') if path.is_file()]
    hashes = {str(path.relative_to(ROOT)): digest(path) for path in sorted(paths)}
    versions = {}
    for name, arguments in [('goose', [goose, 'version']), ('cc', [cc, '--version'])]:
        command = dict(phase=label, purpose=name + ' version')
        report['provenance_commands'].append(command)
        versions[name] = execute(arguments, command, timeout=30).decode('utf-8', errors='replace').strip()
    try:
        compiler_source = git_identity(goose.parent)
    except subprocess.CalledProcessError:
        compiler_source = dict(unavailable=True)
    return dict(source_git=git_identity(ROOT), source_hashes=hashes,
                native_fingerprint=native_fingerprint(), manifest=(ROOT / 'process.build').read_text(),
                tool_versions=versions,
                tools={name: dict(path=str(path), sha256=digest(path)) for name, path in [('goose', goose), ('cc', cc)]},
                goose_source_git=compiler_source,
                required_goose_revision=(ROOT / 'GOOSE_REVISION').read_text().strip(),
                binaries={path.name: dict(path=str(path), sha256=digest(path)) for path in binaries})


def check_protocol(output, case):
    lines = output.splitlines()
    if len(lines) != 1:
        raise ValueError('expected one benchmark row')
    fields = lines[0].split()
    if len(fields) != 4 or any(not value.isdigit() for value in fields):
        raise ValueError('expected four unsigned decimal benchmark fields')
    wall, out, first, second = map(int, fields)
    expected = (3, 0, 0) if case == 'tiny' else (327680, 65536, 65536)
    if wall <= 0 or (out, first, second) != expected:
        raise ValueError('invalid duration or capture lengths')
    return dict(wall_ns=wall, stdout_bytes=out, first_stderr_bytes=first, second_stderr_bytes=second)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--goose', required=True)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--output', type=Path, default=ROOT / '.build/pipeline-measure.json')
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--build-only', action='store_true', help='build without running the driver')
    mode.add_argument('--probe-only', action='store_true', help='build and check four unmeasured cells; dirty sources allowed')
    args = parser.parse_args()
    report = dict(status='incomplete', timestamp=timestamp(), mode='build' if args.build_only else 'probe' if args.probe_only else 'measure',
                  platform=platform.platform(), cpu_affinity=sorted(os.sched_getaffinity(0)),
                  load_start=list(os.getloadavg()),
                  cpu_model=next((line.split(':', 1)[1].strip() for line in Path('/proc/cpuinfo').read_text().splitlines()
                                  if line.startswith('model name')), platform.machine()),
                  seed=SEED, rounds=9, calls_per_cell=5, warmups_per_cell=1,
                  planned_measured_calls=180, planned_warmup_calls=36,
                  preparation=[], provenance_commands=[], probes=[], samples=[],
                  cases=dict(tiny=dict(input_bytes=3, prefix_bytes=0, stdout_bytes=3, first_stderr_bytes=0, second_stderr_bytes=0),
                             pressure=dict(input_bytes=262144, prefix_bytes=65536, stdout_bytes=327680,
                                           first_stderr_bytes=65536, second_stderr_bytes=65536)),
                  options=dict(timeout_ms_both=30000, producer_stdin='Bytes', consumer_stdin='reserved default Null',
                               producer_stdout='reserved Default with default stdout_limit/stdout_fd',
                               first_stderr_limit=65536, second_stderr_limit=65536,
                               second_stdout_limit='exact expected stdout length'),
                  scope='Same managed pipeline implementation: run_pipeline copies three prefixes; '
                        'start_pipeline/wait_pipeline borrows them. One fresh owner per driver process; no capacity reuse. '
                        'Shared-host successful finite-input cases, not safety-policy parity, interactivity, a cross-language '
                        'comparison, universal speed evidence or memory evidence. Driver timer includes owner/caller-buffer '
                        'initialization, launch, input copy, pumping, capture allocation, binary validation, EOF/reap completion '
                        'and cleanup; convenience output copying is naturally included. '
                        'The owner route also performs a terminal zero-slice step and explicit close; these are complete '
                        'API routes, not an isolated copy-cost experiment. Input/argv/options construction, '
                        'driver startup and build are excluded from driver wall_ns. Auxiliary process CPU and wall include '
                        'driver startup and are separately labeled. No memory metric or ranking. '
                        'One standalone process per warmup/call; each cell has one warmup followed by five calls; '
                        'four cells shuffled independently in each of nine rounds. All raw output/status/order retained.')
    try:
        controller.controller_scope()
        goose, cc = controller.executable(args.goose), controller.executable(args.cc)
        report['before_build'] = snapshot(goose, cc, report, 'before_build')
        if report['mode'] == 'measure' and report['before_build']['source_git']['status']:
            raise ValueError('commit benchmark source before measuring')
        if any(line and (line[0].islower() or line[0] == 'S') for line in controller.git('ls-files', '-v').splitlines()):
            raise ValueError('source index hides changes')
        directory = ROOT / '.build/pipeline-bench'
        directory.mkdir(parents=True, exist_ok=True)
        worker, binary = directory / 'pipeline-helper', directory / 'driver'
        commands = [[cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', ROOT / 'tests/pipeline_helper.c', '-o', worker],
                    [goose, 'build', ROOT / 'bench/pipeline.goose', '--cc', cc, '-O2', '-o', binary]]
        report['build_flags'] = commands = [list(map(str, command)) for command in commands]
        for arguments in commands:
            record = {}; report['preparation'].append(record)
            execute(arguments, record, timeout=180)
        report['before_calls'] = snapshot(goose, cc, report, 'before_calls', [worker, binary])
        if {key: value for key, value in report['before_calls'].items() if key != 'binaries'} != {
                key: value for key, value in report['before_build'].items() if key != 'binaries'}:
            raise RuntimeError('source/tool identities changed during build')
        before_fds = len(list(Path('/proc/self/fd').iterdir()))
        if args.probe_only:
            schedule = [dict(route=route, case=case, kind='unmeasured_probe') for route, case in CELLS]
            report['probes'] = schedule
        elif args.build_only:
            schedule = []
        else:
            schedule = []; rng = random.Random(SEED)
            for round_number in range(9):
                cells = CELLS.copy(); rng.shuffle(cells)
                for position, (route, case) in enumerate(cells):
                    for call in range(6):
                        schedule.append(dict(round=round_number, cell_position=position, route=route, case=case,
                                             kind='warmup' if call == 0 else 'measured', call_index=call))
            report['samples'] = schedule
        for position, sample in enumerate(schedule):
            sample.update(order=position, status='pending')
        for sample in schedule:
            output = execute([binary, sample['route'], sample['case']], sample)
            try:
                values = check_protocol(output, sample['case'])
            except BaseException as error:
                sample.update(status='failed', protocol_error=str(error))
                raise
            # Probe timings are intentionally not admitted as measured samples.
            if sample['kind'] == 'unmeasured_probe':
                values['unmeasured_wall_ns'] = values.pop('wall_ns')
            sample.update(values, passed=True)
        report['controller_fds'] = [before_fds, len(list(Path('/proc/self/fd').iterdir()))]
        if report['controller_fds'][0] != report['controller_fds'][1]:
            raise RuntimeError('controller descriptor growth')
        report['after_calls'] = snapshot(goose, cc, report, 'after_calls', [worker, binary])
        if report['before_calls'] != report['after_calls']:
            raise RuntimeError('source/tool/binary identities changed during calls')
        report['status'] = 'complete'
    except BaseException as error:
        report.update(status='failed', error_kind=type(error).__name__, error=str(error), traceback=traceback.format_exc())
        raise
    finally:
        try:
            if 'after_calls' not in report and 'goose' in locals() and 'cc' in locals():
                try:
                    binaries = [path for path in (ROOT / '.build/pipeline-bench').glob('*') if path.is_file()]
                    report['after_failure'] = snapshot(goose, cc, report, 'after_failure', binaries)
                except BaseException as error:
                    report['after_failure_error'] = dict(error_kind=type(error).__name__, error=str(error))
            report['final_adopted_children_recovered'] = controller.adopted_cleanup()
            if report['final_adopted_children_recovered']:
                report['status'] = 'failed'
                raise RuntimeError('final benchmark cleanup found adopted children')
        finally:
            report['measured_calls'] = sum(sample.get('passed', False) and sample['kind'] == 'measured'
                                           for sample in report['samples'])
            report['warmup_calls'] = sum(sample.get('passed', False) and sample['kind'] == 'warmup'
                                         for sample in report['samples'])
            report['probe_calls'] = sum(sample.get('passed', False) for sample in report['probes'])
            report.update(finished_at=timestamp(), load_finish=list(os.getloadavg()))
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
