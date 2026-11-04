#!/usr/bin/env python3
"""Retain raw end-to-end process samples and their build context."""
# SPDX-License-Identifier: Apache-2.0
import argparse
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
    args = parser.parse_args()
    if not 1 <= args.iterations <= 100000 or not 1 <= args.runs <= 100:
        parser.error('iterations/runs outside supported bounds')
    samples = {'capture': [], 'inherit': []}
    raw = []
    for _ in range(args.runs):
        output = command(str(ROOT / '.build/bench'), str(args.iterations))
        raw.append(output)
        for line in output.splitlines():
            mode, value = line.split()
            samples[mode].append(int(value))
    cpu = next((s.partition(':')[2].strip() for s in Path('/proc/cpuinfo').read_text().splitlines()
                if s.startswith('model name')), platform.machine())
    report = dict(timestamp=datetime.now(timezone.utc).isoformat(),
        platform=platform.platform(), cpu=cpu, goose=command(args.goose, 'version'),
        compiler=command(args.cc, '--version'), flags=['-O2'],
        source_revision=command('git', 'rev-parse', 'HEAD'),
        source_dirty=bool(command('git', 'status', '--porcelain')),
        source_fingerprint=(ROOT / 'process.build').read_text(),
        iterations=args.iterations, runs=args.runs, unit='nanoseconds per call',
        samples=samples, raw_stdout=raw,
        medians={k: statistics.median(v) for k, v in samples.items()})
    destination = ROOT / '.build/bench.json'
    destination.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Benchmark samples saved to {destination}')

if __name__ == '__main__':
    main()
