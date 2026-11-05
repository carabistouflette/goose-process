#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check fixed adapters' byte oracles and cleanup after wrong worker behavior."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time

import measure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--goose', default=str(measure.ROOT / '.toolchain/goose/build/goose'))
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--deadlines', action='store_true', help='also run8 real30-second deadline/cleanup checks')
    parser.add_argument('--deadline-output', type=Path, default=measure.ROOT / '.build/duplex-deadlines.json')
    args = parser.parse_args()
    measure.controller.controller_scope()
    try:
        worker, commands, identities, versions = measure.build(args)
        directory = worker.parent
        measure.controller.command([measure.controller.executable(args.cc), '-std=c11', '-Wall', '-Wextra',
                                    '-Werror', '-O2', measure.HERE / 'fault.c', '-o', directory / 'fault'])
        for name in ('early', 'corrupt', 'excess', 'frontload'):
            link = directory / name
            link.unlink(missing_ok=True)
            os.link(directory / 'fault', link)
        for command in commands.values():
            for size in (31, 4096):
                measure.checked(command, worker, size)
            for name in ('early', 'corrupt', 'excess', 'frontload'):
                code, _, _ = measure.controller.run_owned([*command, directory / name, '31', '128'],
                                                          stderr=subprocess.PIPE)
                adopted = measure.controller.adopted_cleanup()
                if code == 0 or adopted:
                    raise RuntimeError(f'failed oracle or cleanup for {name}: status{code}, adopted{adopted}')
        partial = directory / 'watchdog.py'
        partial.write_text('import os,time\nos.write(1,b"partial stdout\\n")\n'
                           'os.write(2,b"partial stderr\\n")\ntime.sleep(60)\n')
        try:
            measure.checked([sys.executable, partial], worker, 31, timeout=.1)
        except measure.InvocationFailed as error:
            result = error.result
            if (not result['watchdog_timeout'] or result['stdout'] != 'partial stdout\n' or
                    result['stderr'] != 'partial stderr\n' or result['adopted_cleanup_count']):
                raise RuntimeError('watchdog diagnostics/cleanup were lost')
        else:
            raise RuntimeError('watchdog fixture was accepted')
        print('8 successful and16 rejected duplex adapter cases passed; no adopted children')
        print('Watchdog partial stdout/stderr retained after private-group cleanup')
        if args.deadlines:
            revision = measure.controller.git('rev-parse', 'HEAD')
            initial_status = measure.controller.git('status', '--porcelain')
            identities.update(check=Path(__file__), fault=measure.HERE / 'fault.c', fault_binary=directory / 'fault')
            hashes = {name: measure.digest(path) for name, path in identities.items()}
            report = dict(status='incomplete', source_revision=revision, source_dirty=bool(initial_status),
                          timestamp=datetime.now(timezone.utc).isoformat(), cpu_affinity=sorted(os.sched_getaffinity(0)),
                          hashes=hashes, versions=versions, samples=[],
                          scope='Eight concurrent fixed-adapter30s deadline probes, not timing comparison. '
                          'Idle before first response and idle after protocol EOF; private groups/direct-child cleanup. '
                          'stderr checkout prefixes normalized; original bytes separately SHA256-fingerprinted.')
            for name in ['idle', 'after-eof']:
                link = directory / name; link.unlink(missing_ok=True); os.link(directory / 'fault', link)
            def probe(cell):
                language, mode = cell; began = time.monotonic()
                code, out, errors = measure.controller.run_owned(
                    [*commands[language], directory / mode, '31', '128'], timeout=45, stderr=subprocess.PIPE)
                elapsed = time.monotonic() - began
                return dict(language=language, mode=mode, elapsed_seconds=elapsed, returncode=code,
                            stdout=out.decode('ascii', errors='replace'),
                            stderr=errors.decode('utf-8', errors='replace').replace(str(measure.ROOT), '<source>'),
                            raw_stderr_sha256=hashlib.sha256(errors).hexdigest(),
                            passed=code != 0 and 29 <= elapsed < 40)
            try:
                with ThreadPoolExecutor(max_workers=8) as pool:
                    report['samples'] = list(pool.map(probe, [(language, mode) for language in commands
                                                            for mode in ['idle', 'after-eof']]))
                report['adopted_cleanup_count'] = measure.controller.adopted_cleanup()
                if not all(sample['passed'] for sample in report['samples']) or report['adopted_cleanup_count']:
                    raise RuntimeError('adapter deadline/cleanup failed')
                if hashes != {name: measure.digest(path) for name, path in identities.items()}:
                    raise RuntimeError('deadline probe identities changed')
                if measure.controller.git('rev-parse', 'HEAD') != revision or measure.controller.git('status', '--porcelain') != initial_status:
                    raise RuntimeError('deadline probe sources changed')
                report['status'] = 'complete'
            except BaseException as error:
                report.update(status='failed', error_kind=type(error).__name__, error=str(error))
                raise
            finally:
                report['finished_at'] = datetime.now(timezone.utc).isoformat()
                args.deadline_output.parent.mkdir(parents=True, exist_ok=True)
                args.deadline_output.write_text(json.dumps(report, indent=2) + '\n')
            print('8 actual30-second adapter deadlines passed without adopted children')
    finally:
        if measure.controller.adopted_cleanup():
            raise RuntimeError('final adapter cleanup found a child')


if __name__ == '__main__':
    main()
