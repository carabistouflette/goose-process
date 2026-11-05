#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Retain public capture/stream healthy-workload timings, without equal-policy claims."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import checker_consumer as controller


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def measure(args):
    if controller.git('status', '--porcelain'):
        raise ValueError('commit measurement source first')
    goose, cc = controller.executable(args.goose), controller.executable(args.cc)
    output = ROOT / '.build/stream-bench'
    output.mkdir(exist_ok=True)
    helper, binary = output / 'helper', output / 'driver'
    controller.command([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', ROOT / 'tests/helper.c', '-o', helper])
    controller.command([goose, 'build', ROOT / 'bench/stream.goose', '--cc', cc, '-O2', '-o', binary])
    if controller.adopted_cleanup():
        raise RuntimeError('benchmark build left children')
    revision = controller.git('rev-parse', 'HEAD')
    inputs = dict(helper=helper, binary=binary, goose=goose, cc=cc,
                  driver=Path(__file__), source=ROOT / 'bench/stream.goose', manifest=ROOT / 'process.build')
    hashes = {k: digest(v) for k, v in inputs.items()}
    report = dict(status='incomplete', source_revision=revision, hashes=hashes,
                  timestamp=datetime.now(timezone.utc).isoformat(),
                  platform=platform.platform(), python=platform.python_version(),
                  goose_version=controller.git('rev-parse', 'HEAD', cwd=ROOT / '.toolchain/goose'),
                  compiler_revision=(ROOT / 'GOOSE_REVISION').read_text().strip(),
                  cpu_affinity=sorted(os.sched_getaffinity(0)), load_start=list(os.getloadavg()),
                  cpu_model=next((s.split(':', 1)[1].strip() for s in Path('/proc/cpuinfo').read_text().splitlines()
                                  if s.startswith('model name')), platform.machine()),
                  build_flags='helper -std=c11 -Wall -Wextra -Werror -O2; Goose driver --cc selected-compiler -O2',
                  rounds=9, calls_per_cell=5, seed=20261105, samples=[],
                  scope='One shared-host CPU-affinity series; no exclusive isolation or general speed/memory claim. Public APIs, identical successful bytes/status: two independent streams each 64KiB or 1MiB. Capture enforces total caps; Stream does not. Not safety-policy parity. Per-call timer includes public launch/drain/reap and byte validation; driver startup/build excluded. Reusable capture buffers and streamed Child; fixed 16KiB stack spans per stream (4KiB view for stream-4k). One warmup per cell process retained separately.')
    before = len(list(Path('/proc/self/fd').iterdir()))
    rng = random.Random(report['seed'])
    try:
        cells = [(size, route) for size in [65536, 1048576] for route in ['capture', 'stream-4k', 'stream-16k']]
        for round_number in range(9):
            order = cells.copy(); rng.shuffle(order)
            for size, route in order:
                sample = dict(round=round_number, size_per_stream=size, route=route, passed=False)
                report['samples'].append(sample)
                code, data, _ = controller.run_owned([binary, route, str(size), '5'])
                sample['stdout'] = data.decode('ascii', errors='replace')
                if code or controller.adopted_cleanup():
                    raise RuntimeError('benchmark driver failed or retained child')
                rows = [[int(n) for n in line.split()] for line in data.splitlines()]
                if len(rows) != 6 or any(len(row) != 2 or row[0] != i or row[1] <= 0 for i, row in enumerate(rows)):
                    raise RuntimeError('invalid driver timings')
                sample.update(warmup_ns=rows[0][1], calls_ns=[r[1] for r in rows[1:]], passed=True)
            print('Completed streaming round', round_number + 1, flush=True)
        if hashes != {k: digest(v) for k, v in inputs.items()} or controller.git('rev-parse', 'HEAD') != revision or controller.git('status', '--porcelain'):
            raise RuntimeError('measurement inputs changed')
        report['controller_fds'] = [before, len(list(Path('/proc/self/fd').iterdir()))]
        if report['controller_fds'] != [before, before]:
            raise RuntimeError('descriptor growth')
        report['status'] = 'complete'
    except BaseException as error:
        report.update(status='failed', error_kind=type(error).__name__)
        raise
    finally:
        report.update(finished_at=datetime.now(timezone.utc).isoformat(), load_finish=list(os.getloadavg()))
        args.output.write_text(json.dumps(report, indent=2) + '\n')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--goose', required=True)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--output', type=Path, default=ROOT / '.build/stream-measure.json')
    args = parser.parse_args()
    controller.controller_scope()
    try:
        measure(args)
    finally:
        controller.adopted_cleanup()


if __name__ == '__main__':
    main()
