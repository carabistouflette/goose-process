#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Prove soak failures preserve diagnostics and reap private fixture groups."""
import os
from pathlib import Path
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'bench'))
import stream_soak as soak


def run_fixture(body, exception, expected_stderr, adopted=0, expected_stdout=b''):
    with tempfile.TemporaryDirectory(dir=ROOT / '.build') as temp:
        fixture = Path(temp) / 'fixture'
        fixture.write_text('#!' + sys.executable + '\nimport os,sys,time\n' + body)
        fixture.chmod(0o700)
        session = soak.Session(fixture)
        try:
            assert session.line() == 'ready'
            session.send('next')
            try:
                session.line(timeout=.1)
            except exception:
                pass
            else:
                raise AssertionError('invalid soak protocol was accepted')
        finally:
            session.close()
            cleaned = soak.controller.adopted_cleanup()
        if expected_stderr not in session.errors or expected_stdout not in session.raw or cleaned != adopted:
            raise AssertionError('cleanup lost diagnostics or left a fixture child')


def main():
    soak.controller.controller_scope()
    before = len(list(Path('/proc/self/fd').iterdir()))
    run_fixture('print("ready",flush=True)\nsys.stdin.readline()\n'
                'os.write(2,b"fixture assertion diagnostic\\n")\nos._exit(1)\n',
                RuntimeError, b'fixture assertion diagnostic\n')
    run_fixture('print("ready",flush=True)\nsys.stdin.readline()\n'
                'os.write(1,b"x"*5000)\nos._exit(1)\n', RuntimeError, b'', expected_stdout=b'x' * 5000)
    run_fixture('child=os.fork()\nif child==0:\n time.sleep(60)\n os._exit(0)\n'
                'print("ready",flush=True)\nsys.stdin.readline()\ntime.sleep(60)\n',
                TimeoutError, b'', adopted=1)
    if len(list(Path('/proc/self/fd').iterdir())) != before:
        raise AssertionError('soak controller descriptor growth')
    print('Soak diagnostic-retention, malformed-output and group-timeout fixtures passed')


if __name__ == '__main__':
    main()
