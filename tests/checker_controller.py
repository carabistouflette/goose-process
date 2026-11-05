#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Exercise test-controller cleanup with real children and injected fd failure."""
import errno
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import checker_consumer as consumer


def require_reaped(pid):
    try:
        os.waitpid(pid, os.WNOHANG)
    except ChildProcessError:
        return
    raise AssertionError('controller retained a child')


def main():
    consumer.controller_scope()
    before = len(list(Path('/proc/self/fd').iterdir()))
    try:
        children = []
        for _ in range(2):
            pid = os.fork()
            if pid == 0:
                time.sleep(30)
                os._exit(0)
            children.append(pid)
        failure = OSError(errno.EMFILE, 'injected pidfd allocation failure')
        with patch.object(consumer.os, 'pidfd_open', side_effect=failure) as opening:
            try:
                consumer.adopted_cleanup()
            except OSError as error:
                if error.errno != errno.EMFILE:
                    raise
            else:
                raise AssertionError('controller hid its allocation failure')
            if opening.call_count != 2:
                raise AssertionError('controller skipped an owned child')
        for pid in children:
            require_reaped(pid)

        with tempfile.TemporaryDirectory(dir=ROOT / '.build') as temp:
            marker = Path(temp) / 'descendant'
            program = ('import os,time,pathlib; child=os.fork(); '
                       'pathlib.Path(' + repr(str(marker)) + ').write_text(str(child)) '
                       'if child else None; time.sleep(30)')
            try:
                consumer.command([sys.executable, '-c', program], timeout=1)
            except subprocess.TimeoutExpired:
                pass
            else:
                raise AssertionError('build timeout was not reported')
            descendant = int(marker.read_text())
            if consumer.adopted_cleanup() != 1:
                raise AssertionError('timed-out build descendant was not adopted')
            require_reaped(descendant)

        calls = []
        def capture(arguments, **kwargs):
            calls.append((arguments, kwargs))
        inherited = dict(SANITIZE='1', MAKEFLAGS='n -- SANITIZE=1',
                         GNUMAKEFLAGS='-n', MFLAGS='-n', MAKEOVERRIDES='SANITIZE=1')
        with patch.dict(os.environ, inherited), patch.object(consumer, 'command', capture):
            consumer.setup(Path('/goose'), Path('/cc'))
        arguments, options = calls[0]
        if 'SANITIZE=0' not in arguments or any(name in options['env'] for name in inherited if name != 'SANITIZE'):
            raise AssertionError('nested build inherited instrumentation or Make controls')
    finally:
        consumer.adopted_cleanup()
    if len(list(Path('/proc/self/fd').iterdir())) != before:
        raise AssertionError('controller descriptor growth')
    print('Controller fd-failure, build-tree timeout and normal-build provenance probes passed')


if __name__ == '__main__':
    main()
