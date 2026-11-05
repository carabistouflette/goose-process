#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bound trailing diagnostics and prove failed resident fixtures are reaped."""
import os
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'bench'))
import duplex_soak as soak


def fixture(body, *, trailing=False, adopted=0, expected_error=RuntimeError):
    with tempfile.TemporaryDirectory(dir=ROOT / '.build') as temp:
        path = Path(temp) / 'fixture'
        path.write_text('#!' + sys.executable + '\nimport os,sys,time\n' + body)
        path.chmod(0o700)
        session = soak.Session(path, output_cap=4096, error_cap=4096)
        caught = False
        try:
            assert session.line() == 'ready'
            session.send('next')
            try:
                if trailing:
                    session.finish()
                else:
                    session.line(timeout=.1)
            except expected_error:
                caught = True
        finally:
            try:
                session.close()
            except RuntimeError:
                if not session.overflow:
                    raise
            cleaned = soak.controller.adopted_cleanup()
        if not caught or cleaned != adopted or len(session.raw) > 4096 or len(session.errors) > 4096:
            raise AssertionError('failed resident fixture escaped cleanup/retention contract')
        if session.proc.returncode is None:
            raise AssertionError('resident leader was not reaped')
        return session


def main():
    (ROOT / '.build').mkdir(exist_ok=True)
    soak.controller.controller_scope()
    before = len(list(Path('/proc/self/fd').iterdir()))
    diagnostic = fixture('print("ready",flush=True)\nsys.stdin.readline()\n'
                         'os.write(2,b"intentional fixture failure\\n")\nos._exit(1)\n')
    if b'intentional fixture failure\n' not in diagnostic.errors:
        raise AssertionError('fixture diagnostic was lost')
    fixture('child=os.fork()\nif child==0:\n time.sleep(60)\n os._exit(0)\n'
            'print("ready",flush=True)\nsys.stdin.readline()\ntime.sleep(60)\n',
            adopted=1, expected_error=TimeoutError)
    for fd in [1, 2]:
        overflow = fixture('print("ready",flush=True)\nsys.stdin.readline()\n'
                           f'os.write({fd},b"x"*65536)\nos._exit(0)\n', trailing=True)
        if not overflow.overflow:
            raise AssertionError('trailing-output overflow was accepted')
    if len(list(Path('/proc/self/fd').iterdir())) != before:
        raise AssertionError('resident controller descriptor growth')
    print('Duplex controller bounded trailing diagnostics and private-child cleanup passed')


if __name__ == '__main__':
    main()
