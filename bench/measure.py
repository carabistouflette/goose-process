#!/usr/bin/env python3
"""Retain raw end-to-end process samples and their build context."""
# SPDX-License-Identifier: Apache-2.0
import argparse
import hashlib
import json
from pathlib import Path
import platform
import statistics
import subprocess
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parent.parent
def command(*args):
    return subprocess.check_output(args, cwd=ROOT, text=True, timeout=120).strip()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iterations', type=int, default=100)
    parser.add_argument('--runs', type=int, default=7)
    parser.add_argument('--goose', required=True)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--executable', type=Path, default=ROOT / '.build/bench')
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--baseline-revision')
    parser.add_argument('--output', type=Path, default=ROOT / '.build/bench.json')
    args = parser.parse_args()
    if not 1 <= args.iterations <= 100000 or not 1 <= args.runs <= 100:
        parser.error('iterations/runs outside supported bounds')
    modes = ['capture', 'inherit', 'maxcap-empty', 'streams-64k', 'streams-1m', 'large-argv']
    executables = {'current': args.executable.resolve()}
    if args.baseline: executables['baseline'] = args.baseline.resolve()
    samples = {variant: {mode: [] for mode in modes} for variant in executables}
    raw = []
    for run in range(args.runs):
        order = list(executables) if run % 2 == 0 else list(reversed(executables))
        for variant in order:
            output = command(str(executables[variant]), str(args.iterations))
            raw.append(dict(run=run, variant=variant, stdout=output))
            values = {}
            for line in output.splitlines():
                mode, value = line.split()
                if mode not in modes or mode in values or int(value) < 0:
                    raise ValueError('invalid or duplicate benchmark fixture')
                values[mode] = int(value)
            if set(values) != set(modes):
                raise ValueError('benchmark output omitted a fixture')
            for mode, value in values.items(): samples[variant][mode].append(value)
    cpu = next((s.partition(':')[2].strip() for s in Path('/proc/cpuinfo').read_text().splitlines()
                if s.startswith('model name')), platform.machine())
    report = dict(timestamp=datetime.now(timezone.utc).isoformat(),
        platform=platform.platform(), cpu=cpu, goose=command(args.goose, 'version'),
        compiler=command(args.cc, '--version'), flags=['-O2'],
        source_revision=command('git', 'rev-parse', 'HEAD'),
        source_dirty=bool(command('git', 'status', '--porcelain')),
        source_fingerprint=(ROOT / 'process.build').read_text(),
        iterations=args.iterations, runs=args.runs, unit='nanoseconds per call',
        samples=samples['current'], raw_stdout=raw,
        executable_hashes={k: hashlib.sha256(v.read_bytes()).hexdigest() for k, v in executables.items()},
        medians={k: statistics.median(v) for k, v in samples['current'].items()})
    if args.baseline:
        report['baseline'] = dict(revision=args.baseline_revision, samples=samples['baseline'],
            medians={k: statistics.median(v) for k, v in samples['baseline'].items()})
    destination = args.output
    destination.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Benchmark samples saved to {destination}')

if __name__ == '__main__':
    main()
