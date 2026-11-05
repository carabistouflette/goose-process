#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Use a pinned external fixture checker on the actual library regressions."""
import argparse
import ctypes
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
FIXTURES = ROOT / 'tests/consumer'
OUTPUT = ROOT / '.build/checker-consumer'
REVISION = (ROOT / 'CHECKER_REVISION').read_text().strip()
SOURCE = ROOT / '.build' / ('goose-check-' + REVISION)


def executable(value):
    found = shutil.which(str(value))
    if not found:
        raise ValueError('executable was not found')
    return Path(found).resolve()


def command(arguments, cwd=ROOT, timeout=180, env=None):
    with subprocess.Popen(list(map(str, arguments)), cwd=cwd, env=env,
                          start_new_session=True) as proc:
        try:
            code = proc.wait(timeout=timeout)
            if code:
                raise subprocess.CalledProcessError(code, arguments)
        finally:
            if proc.returncode is None:
                try:
                    os.killpg(proc.pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
                proc.wait()


def git(*arguments, cwd=ROOT):
    return subprocess.check_output(['git', *arguments], cwd=cwd, timeout=30,
                                   text=True).strip()


def clean_source(path, revision):
    if path.is_symlink() or Path(git('rev-parse', '--show-toplevel', cwd=path)).resolve() != path.resolve():
        raise ValueError('checker source must be its own checkout, not a symlink or nested directory')
    if git('rev-parse', 'HEAD', cwd=path) != revision:
        raise ValueError('checker source differs from the pinned revision')
    flags = git('ls-files', '-v', cwd=path).splitlines()
    if any(line and (line[0].islower() or line[0] == 'S') for line in flags):
        raise ValueError('checker index hides source changes')
    if git('status', '--porcelain', '--untracked-files=normal', cwd=path):
        raise ValueError('checker source is modified; preserve it before setup')


def setup(goose, cc):
    if len(REVISION) != 40 or any(c not in '0123456789abcdef' for c in REVISION):
        raise ValueError('CHECKER_REVISION requires a complete lowercase Git revision')
    SOURCE.parent.mkdir(exist_ok=True)
    if not SOURCE.exists():
        # Only a complete pinned snapshot enters the reusable location. Failed
        # fetches are confined to this new, private temporary directory.
        with tempfile.TemporaryDirectory(prefix='checker-fetch-', dir=SOURCE.parent) as temp:
            tree = Path(temp) / 'source'
            command(['git', 'init', str(tree)])
            command(['git', 'remote', 'add', 'origin',
                     'https://github.com/carabistouflette/goose-check.git'], cwd=tree)
            command(['git', 'fetch', '--no-auto-maintenance', '--depth', '1', 'origin', REVISION], cwd=tree)
            command(['git', 'checkout', '--detach', REVISION], cwd=tree)
            clean_source(tree, REVISION)
            tree.rename(SOURCE)
    clean_source(SOURCE, REVISION)
    if (SOURCE / 'GOOSE_REVISION').read_bytes() != (ROOT / 'GOOSE_REVISION').read_bytes():
        raise ValueError('checker and library require different compiler revisions')
    build_env = os.environ.copy()
    for name in ['MAKEFLAGS', 'GNUMAKEFLAGS', 'MFLAGS', 'MAKEOVERRIDES']:
        build_env.pop(name, None)
    command(['make', 'build', 'SANITIZE=0', 'GOOSE=' + str(goose), 'CC=' + str(cc)],
            cwd=SOURCE, timeout=300, env=build_env)
    clean_source(SOURCE, REVISION)
    print('Built pinned goose-check ' + REVISION)


def controller_scope():
    # This is a standalone, single-threaded test controller with no external
    # waiters. Adopt only orphaned fixtures, not children of living owners.
    signal.signal(signal.SIGCHLD, signal.SIG_DFL)
    libc = ctypes.CDLL(None, use_errno=True)
    libc.prctl.argtypes = [ctypes.c_int] + [ctypes.c_ulong] * 4
    libc.prctl.restype = ctypes.c_int
    if libc.prctl(36, 1, 0, 0, 0):  # PR_SET_CHILD_SUBREAPER
        raise OSError(ctypes.get_errno(), 'cannot establish test subreaper')


def adopted_cleanup():
    count = 0
    first_error = None
    while True:
        children = Path('/proc/self/task', str(os.getpid()), 'children').read_text().split()
        if not children:
            if first_error is not None:
                raise first_error
            return count
        for text in children:
            pid = int(text)
            # Every listed PID is this controller's unreaped child. No other
            # waiter can release its reservation before pidfd acquisition.
            try:
                fd = os.pidfd_open(pid)
            except OSError as error:
                # Exclusively owned, unreaped children retain their numeric
                # identities until our wait. Allocation failure cannot bypass
                # signaling/reaping them; still fail validation after cleanup.
                if first_error is None:
                    first_error = error
                try:
                    os.kill(pid, signal.SIGKILL)
                except ProcessLookupError:
                    pass
            else:
                try:
                    try:
                        signal.pidfd_send_signal(fd, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    except OSError as error:
                        if first_error is None:
                            first_error = error
                        os.kill(pid, signal.SIGKILL)
                finally:
                    os.close(fd)
            while True:
                try:
                    waited, _ = os.waitpid(pid, 0)
                    if waited == pid:
                        break
                except InterruptedError:
                    continue
            count += 1


def run_owned(arguments, input_bytes=b'', timeout=45, stderr=None):
    proc = subprocess.Popen(list(map(str, arguments)), cwd=ROOT,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=stderr, start_new_session=True)
    try:
        out, errors = proc.communicate(input_bytes, timeout=timeout)
        return proc.returncode, out, errors
    finally:
        if proc.returncode is None:
            # communicate has not reaped this private leader on timeout. Keep
            # the reservation until the entire private group is signaled.
            try:
                os.killpg(proc.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            proc.communicate(timeout=10)


def build(goose, cc):
    OUTPUT.mkdir(parents=True, exist_ok=True)
    command([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-O2',
             ROOT / 'tests/helper.c', '-o', OUTPUT / 'helper'])
    programs = {}
    for name in ['api', 'features']:
        for mode in ['release', 'debug']:
            destination = OUTPUT / (name + '-' + mode)
            flags = ['-O2'] if mode == 'release' else ['--debug', '--no-bce']
            command([goose, 'build', ROOT / ('tests/' + name + '.goose'),
                     '--cc', cc, *flags, '-o', destination])
            programs[name + '-' + mode] = destination
    destination = OUTPUT / 'example'
    command([goose, 'build', ROOT / 'examples/command.goose', '--cc', cc,
             '-O2', '-o', destination])
    programs['example'] = destination
    destination = OUTPUT / 'compiler'
    command([goose, 'build', ROOT / 'examples/compiler.goose', '--cc', cc,
             '-O2', '-o', destination])
    programs['compiler'] = destination
    return programs


def checked(checker, options, arguments, expected_code=0, adopted=0, error=None):
    try:
        code, output, errors = run_owned([checker, '--config', FIXTURES / 'defaults.toml',
                                         *options, '--', *arguments],
                                        stderr=subprocess.PIPE if error else None)
    finally:
        cleaned = adopted_cleanup()
    if code != expected_code or output or cleaned != adopted:
        raise AssertionError(('consumer outcome', code, expected_code,
                              'stdout bytes', len(output), 'adopted fixtures', cleaned, adopted))
    if error and error not in errors:
        raise AssertionError(('missing checker error classification', error, errors))
    if errors:
        sys.stderr.buffer.write(errors)


def exercise(checker, programs):
    for name, binary in programs.items():
        kind = name.split('-')[0]
        checked(checker, ['--stdout', FIXTURES / (kind + '.stdout')], [binary],
                adopted=1 if kind == 'api' else 0)
        print('Checked real ' + name)
    with tempfile.TemporaryDirectory(prefix='checker fixtures ', dir=OUTPUT) as temp:
        temp = Path(temp).resolve()
        binary = temp / 'binary'; binary.write_bytes(bytes(range(256)) * 256)
        arguments = ['', 'a b', ';literal', '--flag']
        oracle = temp / 'argv'; oracle.write_bytes(b''.join(a.encode()+b'\0' for a in arguments))
        helper = OUTPUT / 'helper'
        checked(checker, ['--stdout', oracle], [helper, 'args', *arguments])
        checked(checker, ['--stdin', binary, '--stdout', binary], ['/bin/cat'])
        checked(checker, [], [helper, 'stdin'])
        checked(checker, ['--expect-exit', '127'], [helper, 'exit', '127'])
        # Controls prove that the wrapper actually rejects bad oracles/statuses.
        checked(checker, ['--stdout', oracle], [helper, 'exit'], expected_code=1, error=b'stdout mismatch')
        checked(checker, [], [helper, 'exit', '127'], expected_code=1, error=b'exit mismatch')
        checked(checker, ['--output-limit', '0', '--stdin', binary], ['/bin/cat'],
                expected_code=125, error=b'OutputLimit')
        marker = temp / 'started'; marker.write_bytes(b'')
        checked(checker, ['--timeout-ms', '1000'], [helper, 'close-sleep', marker],
                expected_code=125, error=b'TimedOut')
        if marker.read_bytes() != b'C':
            raise AssertionError('timeout control did not reach its launched fixture')
    print('Binary/stdin/exit fixtures and four rejection controls passed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--goose', required=True)
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--checker', type=Path, default=SOURCE / '.build/goose-check')
    parser.add_argument('--setup', action='store_true')
    args = parser.parse_args()
    goose, cc = executable(args.goose), executable(args.cc)
    controller_scope()
    if args.setup:
        try:
            setup(goose, cc)
        finally:
            if adopted_cleanup():
                raise AssertionError('checker build left adopted children')
        return
    checker = executable(args.checker)
    before = len(list(Path('/proc/self/fd').iterdir()))
    try:
        exercise(checker, build(goose, cc))
    finally:
        leftovers = adopted_cleanup()
    if leftovers or len(list(Path('/proc/self/fd').iterdir())) != before:
        raise AssertionError('consumer controller retained resources')


if __name__ == '__main__':
    main()
