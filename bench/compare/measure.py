#!/usr/bin/env python3
"""Build and interleave checked successful-workload stdlib comparisons."""
# SPDX-License-Identifier: Apache-2.0
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import statistics
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
MODES = ('empty', 'args', 'streams', 'input', 'environment', 'redirect')


def run(command):
    return subprocess.check_output([str(x) for x in command], cwd=ROOT, text=True, timeout=120).strip()


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--goose', default='.toolchain/goose/build/goose')
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--cpu', type=int, default=0)
    parser.add_argument('--rounds', type=int, default=9)
    parser.add_argument('--iterations', type=int, default=40)
    parser.add_argument('--build-only', action='store_true')
    parser.add_argument('--shared-kernel', action='store_true',
                        help='diagnostic Rust front end using the identical C engine and safety policies')
    parser.add_argument('--output', type=Path, default=ROOT / '.build/comparison.json')
    args = parser.parse_args()
    if not 1 <= args.rounds <= 100 or not 1 <= args.iterations <= 10000:
        parser.error('rounds/iterations outside supported bounds')
    if args.cpu not in os.sched_getaffinity(0):
        parser.error('CPU not available in current affinity')
    os.sched_setaffinity(0, {args.cpu})
    goose = (ROOT / args.goose).resolve()
    build = ROOT / '.build'
    build.mkdir(exist_ok=True)
    fixture = build / 'compare-fixture'
    run([sys.executable, ROOT / 'scripts/native-manifest.py', '--check'])
    sources = [ROOT / 'process.goose', ROOT / 'process.build', ROOT / 'GOOSE_REVISION',
               *sorted((ROOT / 'native').glob('*.[ch]')), *sorted(HERE.glob('*'))]
    sources = [path for path in sources if path.is_file()]
    source_hashes = {str(path.relative_to(ROOT)): digest(path) for path in sources}
    revision = run(['git', 'rev-parse', 'HEAD'])
    dirty = bool(run(['git', 'status', '--porcelain']))

    def check_sources():
        if source_hashes != {str(path.relative_to(ROOT)): digest(path) for path in sources}:
            raise RuntimeError('sources changed during build or measurement')
        if revision != run(['git', 'rev-parse', 'HEAD']):
            raise RuntimeError('source revision changed during build or measurement')

    run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', HERE / 'fixture.c', '-o', fixture])
    run([goose, 'build', HERE / 'driver.goose', '--cc', args.cc, '-O2', '-o', build / 'compare-goose'])
    binaries = [fixture, build / 'compare-goose']
    if args.shared_kernel:
        objects = []
        for source in ['process', 'launch']:
            target = build / f'compare-{source}.o'
            run([args.cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-fPIC', '-c',
                 ROOT / 'native' / f'{source}.c', '-o', target])
            objects.append(target)
        run(['ar', 'rcs', build / 'libgpcompare.a', *objects])
        run(['rustc', '-C', 'opt-level=2', HERE / 'shared.rs', '-L', f'native={build}',
             '-l', 'static=gpcompare', '-o', build / 'compare-shared'])
        variants = {'Goose': [build / 'compare-goose'], 'Rust/shared-kernel': [build / 'compare-shared']}
        binaries.append(build / 'compare-shared')
    else:
        run(['rustc', '-C', 'opt-level=2', HERE / 'driver.rs', '-o', build / 'compare-rust'])
        run(['go', 'build', '-trimpath', '-o', build / 'compare-go', HERE / 'driver.go'])
        variants = {'Goose': [build / 'compare-goose'], 'Rust': [build / 'compare-rust'],
                    'Go': [build / 'compare-go'], 'Python': [sys.executable, HERE / 'driver.py']}
        binaries.extend([build / 'compare-rust', build / 'compare-go'])
    check_sources()
    binary_hashes = {str(path.relative_to(ROOT)): digest(path) for path in binaries}
    if args.build_only:
        for mode in MODES:
            for command in variants.values():
                if int(run([*command, fixture, mode, 1])) <= 0:
                    raise ValueError('invalid driver output')
        print(f'All {len(MODES) * len(variants)} driver/fixture checks passed')
        return
    samples = {mode: {variant: [] for variant in variants} for mode in MODES}
    randomizer = random.Random(3025)
    raw = []
    for round_number in range(args.rounds):
        order = [(mode, variant) for mode in MODES for variant in variants]
        randomizer.shuffle(order)
        for mode, variant in order:
            command = [*variants[variant], fixture, mode, args.iterations]
            output = run(command)
            elapsed = int(output)
            if elapsed <= 0:
                raise ValueError('invalid timing')
            samples[mode][variant].append(elapsed)
            raw.append(dict(round=round_number, mode=mode, variant=variant,
                            stdout=output, ns_per_call=elapsed))
    cpu = next((line.partition(':')[2].strip() for line in Path('/proc/cpuinfo').read_text().splitlines()
                if line.startswith('model name')), platform.machine())
    check_sources()
    if binary_hashes != {str(path.relative_to(ROOT)): digest(path) for path in binaries}:
        raise RuntimeError('binaries changed during measurement')
    version_commands = {'Goose': [goose, 'version'], 'C': [args.cc, '--version'],
                        'Rust': ['rustc', '--version', '--verbose'],
                        'Python': [sys.executable, '--version']}
    if not args.shared_kernel:
        version_commands['Go'] = ['go', 'version']
    report = dict(timestamp=datetime.now(timezone.utc).isoformat(), cpu=cpu,
        affinity_cpu=args.cpu, platform=platform.platform(), libc=platform.libc_ver(),
        source_revision=revision, source_dirty=dirty,
        versions={name: run(command) for name, command in version_commands.items()},
        build_flags={'C/Goose': ['-O2'], 'Rust': ['-C', 'opt-level=2'],
            'native_C': ['-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', '-fPIC'],
            **({} if args.shared_kernel else {'Go': ['-trimpath']})},
        iterations=args.iterations, warmups_per_sample=3, rounds=args.rounds, shuffle_seed=3025,
        unit='nanoseconds per successful call, including output verification',
        aggregation='Each sample is a batch mean; medians are medians of batch means, not individual-call latency medians.',
        track='shared-native-kernel diagnostic' if args.shared_kernel else 'native standard-library APIs',
        contract=('Both front ends call the identical native C engine with identical caps/deadlines/signal reset/FD isolation/socket input. Rust wrapper is diagnostic, not an independent Rust process implementation.' if args.shared_kernel else
            'Matched successful argv/environment/stdin/output bytes. Baselines do not implement the complete Goose safety contract: hard caps, finite deadline and reset-all child signals. Python closes foreign FDs by default and restores selected signals; Rust/Go foreign-FD policies differ. Input transport: Goose socket, baseline pipes.'),
        host_context=dict(load_average=os.getloadavg(), environment_entries=len(os.environ),
            environment_bytes=sum(len(os.fsencode(k)) + len(os.fsencode(v)) + 2 for k, v in os.environ.items()),
            scheduler_open_fds=len(list(Path('/proc/self/fd').iterdir())),
            governor=(Path(f'/sys/devices/system/cpu/cpu{args.cpu}/cpufreq/scaling_governor').read_text().strip()
                if Path(f'/sys/devices/system/cpu/cpu{args.cpu}/cpufreq/scaling_governor').exists() else 'unavailable')),
        source_sha256=source_hashes, executable_sha256=binary_hashes,
        samples=samples, raw=raw,
        medians={mode: {variant: statistics.median(values) for variant, values in per_mode.items()}
                 for mode, per_mode in samples.items()})
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Saved {len(raw)} checked samples to {args.output}')
    for mode, values in report['medians'].items():
        print(mode, ', '.join(f'{name}={value / 1000:.1f} us' for name, value in values.items()))


if __name__ == '__main__':
    main()
