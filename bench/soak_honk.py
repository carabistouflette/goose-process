#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bounded, isolated real-Goose service soak through a published Honk binary."""
import argparse
from datetime import datetime, timezone
import fcntl
import hashlib
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time

SOURCE = '''import std;
import os;
extern fn process_id() -> i32 = "getpid";
fn main() {
    let argv = args();
    assert(argv.len == 3);
    if argv[1] == "once" {
        write_stdout("once\\n"); flush_stdout();
        assert(write_file_atomic(argv[2], str(process_id()))); return;
    }
    write_stdout("service-ready\\n"); flush_stdout();
    assert(write_file_atomic(argv[2], str(process_id())));
    for i in 6000 { sleep_ms(10); }
}
'''

def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def service_identity(proc, child):
    candidate = os.pidfd_open(child)
    try:
        if child == proc.pid or os.getsid(child) != proc.pid:
            raise RuntimeError('marker identity is outside the private service session')
    except BaseException:
        os.close(candidate)
        raise
    return candidate

def cleanup(proc, child_fd=None):
    original_error = sys.exc_info()[0] is not None
    failures = []
    def attempt(function):
        try:
            function()
        except ProcessLookupError:
            pass
        except BaseException as error:
            failures.append(error)
    if child_fd is not None:
        try:
            attempt(lambda: signal.pidfd_send_signal(child_fd, signal.SIGKILL))
        finally:
            attempt(lambda: os.close(child_fd))
    if proc.returncode is None:
        if child_fd is None or failures:
            attempt(lambda: os.killpg(proc.pid, signal.SIGTERM))
        try:
            # Give Honk its normal wait/reap path before escalating the leader.
            proc.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            # No wait/poll has reaped this private session leader yet.
            attempt(lambda: os.killpg(proc.pid, signal.SIGKILL))
            attempt(lambda: proc.communicate(timeout=5))
        except BaseException as error:
            failures.append(error)
            if proc.returncode is None:
                attempt(lambda: os.killpg(proc.pid, signal.SIGKILL))
                attempt(lambda: proc.communicate(timeout=5))
    else:
        attempt(lambda: proc.communicate(timeout=5))
    for error in failures:
        print('Soak cleanup failure: '+repr(error), file=sys.stderr)
    if failures and not original_error:
        raise failures[0]

def lock_state(path, held):
    with path.open('r+b') as stream:
        try:
            fcntl.flock(stream, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            if not held:
                raise RuntimeError('lock remains held: ' + str(path))
        else:
            if held:
                raise RuntimeError('lock released while service active: ' + str(path))

def main():
    # Standalone single-threaded controller: owned leader PIDs must not auto-reap.
    # There are no external waiters in this harness.
    signal.signal(signal.SIGCHLD, signal.SIG_DFL)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--honk', type=Path, required=True)
    parser.add_argument('--goose', type=Path, required=True)
    parser.add_argument('--rounds', type=int, default=12)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--consumer-revision', required=True)
    parser.add_argument('--consumer-source', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.rounds <= 100:
        parser.error('rounds must be 1..100')
    honk, goose = args.honk.resolve(), args.goose.resolve()
    identities = {str(p): digest(p) for p in [honk, goose]}
    script = Path(__file__).resolve()
    script_identity = digest(script)
    def verify_source():
        revision = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=args.consumer_source, text=True, timeout=30).strip()
        dirty = subprocess.check_output(['git', 'status', '--porcelain'], cwd=args.consumer_source, text=True, timeout=30)
        if revision != args.consumer_revision or dirty:
            raise RuntimeError('consumer checkout differs from requested clean revision')
    verify_source()
    before_fds = len(list(Path('/proc/self/fd').iterdir()))
    samples = []
    with tempfile.TemporaryDirectory(prefix='goose process soak ') as temporary:
        root = Path(temporary)
        project = root / 'service'
        env = {**os.environ, 'GOOSE': str(goose), 'HONK_HOME': str(root/'cache')}
        def command(*arguments):
            proc = subprocess.Popen([str(honk), *arguments], cwd=project if project.exists() else root,
                                    env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    start_new_session=True)
            try:
                out, err = proc.communicate(timeout=120)
                if proc.returncode:
                    raise subprocess.CalledProcessError(proc.returncode, proc.args, out, err)
                return subprocess.CompletedProcess(proc.args, proc.returncode, out, err)
            finally:
                cleanup(proc)
        command('new', str(project), '--name', 'process_soak')
        (project/'src/main.goose').write_text(SOURCE)
        command('build')
        for index in range(args.rounds):
            # A natural exit and a supervised service share the real build/run path.
            marker = root / f'once-{index}'
            result = command('run', '--', 'once', str(marker))
            if b'once\n' not in result.stdout or not marker.exists():
                raise RuntimeError('natural command output/marker missing')
            samples.append(dict(round=index, mode='natural', returncode=result.returncode))
            marker = root / f'service-{index}'
            child_fd = None
            proc = subprocess.Popen([str(honk), 'run', '--', 'serve', str(marker)], cwd=project,
                                    env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                    start_new_session=True)
            try:
                deadline = time.monotonic() + 30
                while not marker.exists():
                    if time.monotonic() >= deadline:
                        raise TimeoutError('service did not start')
                    time.sleep(0.005)
                child = int(marker.read_text())
                child_fd = service_identity(proc, child)
                locks = [project/'.honk/lock', root/'cache/lock']
                for path in locks:
                    lock_state(path, True)
                # One longer hold tests repeated bounded steps without busy waiting.
                hold = 2.0 if index == 0 else 0.03
                time.sleep(hold)
                for path in locks:
                    lock_state(path, True)
                number = signal.SIGTERM if index % 2 == 0 else signal.SIGINT
                started = time.monotonic()
                proc.send_signal(number)
                out, err = proc.communicate(timeout=10)
                elapsed = time.monotonic() - started
                if proc.returncode != 128 + number or b'service-ready\n' not in out:
                    raise RuntimeError(('cancellation mismatch', proc.returncode, out, err))
                try:
                    signal.pidfd_send_signal(child_fd, 0)
                except ProcessLookupError:
                    pass
                else:
                    raise RuntimeError('service identity still alive after Honk returned')
                for path in locks:
                    lock_state(path, False)
                if b'AddressSanitizer' in err or b'runtime error:' in err:
                    raise RuntimeError(err)
                samples.append(dict(round=index, mode='service', signal=number,
                                    returncode=proc.returncode, held_seconds=hold,
                                    cancellation_return_seconds=elapsed,
                                    child_gone=True, locks_released=True))
            finally:
                cleanup(proc, child_fd)
        command('clean')
    after_fds = len(list(Path('/proc/self/fd').iterdir()))
    if after_fds != before_fds:
        raise RuntimeError('controller descriptor growth')
    try:
        os.waitpid(-1, os.WNOHANG)
    except ChildProcessError:
        pass
    else:
        raise RuntimeError('controller retained an unwaited child')
    if identities != {str(p): digest(p) for p in [honk, goose]}:
        raise RuntimeError('executable changed during soak')
    verify_source()
    if digest(script) != script_identity:
        raise RuntimeError('soak harness changed during run')
    report = dict(timestamp=datetime.now(timezone.utc).isoformat(),
                  consumer_revision=args.consumer_revision,
                  executable_sha256={'honk': identities[str(honk)], 'goose': identities[str(goose)]},
                  harness_sha256=script_identity,
                  goose_version=subprocess.check_output([str(goose), 'version'], text=True, timeout=30).strip(),
                  rounds=args.rounds, samples=samples, controller_fds=[before_fds, after_fds],
                  fixture_sha256=hashlib.sha256(SOURCE.encode()).hexdigest(),
                  scope='Controlled real compiled Goose service via Honk build/run; direct signals only. Short bounded soak, not universal reliability, terminal-launch classification or a memory benchmark.')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(f'Passed {len(samples)} checked runs; report {args.output}')

if __name__ == '__main__':
    main()
