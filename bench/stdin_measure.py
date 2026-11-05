#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Retain successful finite/incremental bulk-input costs, not a general speed claim."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import checker_consumer as controller


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--goose', required=True)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--output', type=Path, default=ROOT / '.build/stdin-measure.json')
    args = parser.parse_args()
    if controller.git('status', '--porcelain'):
        raise ValueError('commit benchmark source before measuring')
    controller.controller_scope()
    goose, cc = controller.executable(args.goose), controller.executable(args.cc)
    directory = ROOT / '.build/stdin-bench'; directory.mkdir(parents=True, exist_ok=True)
    worker, binary = directory / 'worker', directory / 'driver'
    report = None
    try:
        controller.command([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2', ROOT / 'examples/worker.c', '-o', worker])
        controller.command([goose, 'build', ROOT / 'bench/stdin.goose', '--cc', cc, '-O2', '-o', binary])
        if controller.adopted_cleanup():
            raise RuntimeError('preparation retained a child')
        sources = dict(worker=worker, driver=binary, goose=goose, cc=cc, script=Path(__file__),
                       benchmark=ROOT / 'bench/stdin.goose', worker_source=ROOT / 'examples/worker.c',
                       ownership_controller=ROOT / 'scripts/checker_consumer.py', manifest=ROOT / 'process.build')
        identities = {name: digest(path) for name, path in sources.items()}
        revision = controller.git('rev-parse', 'HEAD')
        report = dict(status='incomplete', source_revision=revision, hashes=identities,
                      timestamp=datetime.now(timezone.utc).isoformat(), platform=platform.platform(),
                      cpu_affinity=sorted(os.sched_getaffinity(0)), load_start=list(os.getloadavg()),
                      cpu_model=next((s.split(':', 1)[1].strip() for s in Path('/proc/cpuinfo').read_text().splitlines()
                                      if s.startswith('model name')), platform.machine()),
                      compiler_revision=(ROOT / 'GOOSE_REVISION').read_text().strip(),
                      goose_version=subprocess.check_output([goose, 'version'], text=True, timeout=30).strip(),
                      cc_version=subprocess.check_output([cc, '--version'], text=True, timeout=30).splitlines()[0],
                      flags='worker -std=c11 -Wall -Wextra -Werror -O2; Goose --cc selected -O2',
                      frames=128, input_bytes=524800, stdout_bytes=524800, stderr_bytes=512,
                      output_cap_per_stream=1048576, timeout_ms=30000, rounds=9, calls_per_cell=5,
                      seed=20261105, samples=[],
                      scope='One shared-host successful bulk-input series, no exclusive-core claim. '
                      'Same child bytes/status/captured outputs; synchronous copied Input.Bytes versus caller-driven Input.Stream. '
                      'Input.Stream has no cumulative input cap; Input.Bytes does. Not complete safety-policy parity. '
                      'One warmup per cell process retained separately; driver startup/build/batch construction excluded. '
                      'Per-call timer includes public API calls, draining, reaping, byte validation and incremental close. '
                      'Reusable capture buffers/Child. Not interactive-worker latency, memory evidence or a universal speed comparison.')
        rng = random.Random(report['seed']); before = len(list(Path('/proc/self/fd').iterdir()))
        for round_number in range(9):
            routes = ['finite', 'incremental']; rng.shuffle(routes)
            for route in routes:
                sample = dict(round=round_number, route=route, passed=False); report['samples'].append(sample)
                code, out, err = controller.run_owned([binary, route, '5'], stderr=subprocess.PIPE)
                sample.update(returncode=code, stdout=out.decode('ascii', errors='replace'),
                              stderr=err.decode('utf-8', errors='replace'))
                if code or controller.adopted_cleanup():
                    raise RuntimeError('benchmark failed or retained a child')
                rows = [[int(n) for n in line.split()] for line in out.splitlines()]
                if len(rows) != 6 or any(len(row) != 2 or row[0] != i or row[1] <= 0 for i, row in enumerate(rows)):
                    raise RuntimeError('invalid benchmark protocol')
                sample.update(warmup_ns=rows[0][1], calls_ns=[row[1] for row in rows[1:]], passed=True)
        if identities != {name: digest(path) for name, path in sources.items()} or controller.git('status', '--porcelain') or controller.git('rev-parse', 'HEAD') != revision:
            raise RuntimeError('measurement source/identities changed')
        report['controller_fds'] = [before, len(list(Path('/proc/self/fd').iterdir()))]
        if report['controller_fds'] != [before, before]:
            raise RuntimeError('controller descriptor growth')
        report['status'] = 'complete'
    except BaseException as error:
        if report is not None:
            report.update(status='failed', error_kind=type(error).__name__, error=str(error))
        raise
    finally:
        try:
            if controller.adopted_cleanup():
                if report is not None: report['status'] = 'failed'
                raise RuntimeError('final benchmark cleanup found an adopted child')
        finally:
            if report is not None:
                report.update(finished_at=datetime.now(timezone.utc).isoformat(), load_finish=list(os.getloadavg()))
                args.output.parent.mkdir(parents=True, exist_ok=True)
                args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
