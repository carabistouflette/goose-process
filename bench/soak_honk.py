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
import platform
import selectors
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


def resources(pid):
    """Current Linux process values, not a subprocess spawner's high-water RSS."""
    root = Path('/proc') / str(pid)
    status = root.joinpath('status').read_text()
    values = dict(line.split(':', 1) for line in status.splitlines() if ':' in line)
    return dict(rss_kib=int(values['VmRSS'].split()[0]),
                fds=len(list(root.joinpath('fd').iterdir())))


def drain_for(proc, seconds):
    """Drain both finite fixture streams during its supervised hold, with a cap."""
    captured = [bytearray(), bytearray()]
    deadline = time.monotonic() + seconds
    with selectors.DefaultSelector() as selector:
        for index, stream in enumerate([proc.stdout, proc.stderr]):
            os.set_blocking(stream.fileno(), False)
            selector.register(stream, selectors.EVENT_READ, index)
        while time.monotonic() < deadline:
            for key, _ in selector.select(min(0.02, max(0, deadline-time.monotonic()))):
                try:
                    chunk = os.read(key.fd, 65536)
                except BlockingIOError:
                    continue
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                captured[key.data].extend(chunk)
                if len(captured[key.data]) > 1024*1024:
                    raise RuntimeError('fixture stream exceeded 1 MiB controller cap')
    return tuple(bytes(value) for value in captured)


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
    parser.add_argument('--duration-seconds', type=int, default=0,
                        help='measured duration after warmup; overrides rounds')
    parser.add_argument('--warmup-rounds', type=int, default=0)
    parser.add_argument('--hold-seconds', type=float, default=None)
    parser.add_argument('--pressure', action='store_true',
                        help='write and verify 256 KiB on each service stream')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--consumer-revision', required=True)
    parser.add_argument('--consumer-source', type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.rounds <= 100:
        parser.error('rounds must be 1..100')
    if not 0 <= args.duration_seconds <= 3600 or not 0 <= args.warmup_rounds <= 100:
        parser.error('duration must be 0..3600 and warmup rounds 0..100')
    if args.hold_seconds is not None and not 0.03 <= args.hold_seconds <= 30:
        parser.error('hold seconds must be finite and 0.03..30')
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
    source = SOURCE
    if args.pressure:
        source = source.replace('    for i in 6000',
                                '    for n in 4096 { write_stdout("' + 'X'*64 + '"); '
                                'write_stderr("' + 'Y'*64 + '"); }\n'
                                '    flush_stdout();\n    for i in 6000')
    samples = []
    started_at = datetime.now(timezone.utc).isoformat()
    measured_start = None
    measured_started_at = None
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
        (project/'src/main.goose').write_text(source)
        command('build')
        index = 0
        while True:
            if index == args.warmup_rounds:
                measured_start = time.monotonic()
                measured_started_at = datetime.now(timezone.utc).isoformat()
            measured = index >= args.warmup_rounds
            if measured:
                if args.duration_seconds:
                    if time.monotonic()-measured_start >= args.duration_seconds:
                        break
                elif index-args.warmup_rounds >= args.rounds:
                    break
            # A natural exit and a supervised service share the real build/run path.
            marker = root / f'once-{index}'
            result = command('run', '--', 'once', str(marker))
            if b'once\n' not in result.stdout or not marker.exists():
                raise RuntimeError('natural command output/marker missing')
            samples.append(dict(round=index, measured=measured, mode='natural', returncode=result.returncode))
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
                hold = args.hold_seconds if args.hold_seconds is not None else (2.0 if index == 0 else 0.03)
                prefix_out, prefix_err = drain_for(proc, hold)
                # Honk can reap an unexpectedly exited service during the hold.
                # Reject snapshots if that anchored identity dies around procfs reads.
                signal.pidfd_send_signal(child_fd, 0)
                current = dict(controller=resources(os.getpid()),
                               honk=resources(proc.pid), service=resources(child))
                signal.pidfd_send_signal(child_fd, 0)
                for path in locks:
                    lock_state(path, True)
                number = signal.SIGTERM if index % 2 == 0 else signal.SIGINT
                started = time.monotonic()
                proc.send_signal(number)
                out, err = proc.communicate(timeout=10)
                out, err = prefix_out + out, prefix_err + err
                elapsed = time.monotonic() - started
                if proc.returncode != 128 + number or b'service-ready\n' not in out:
                    raise RuntimeError(('cancellation mismatch', proc.returncode, out, err))
                if args.pressure and (out.count(b'X') != 262144 or err != b'Y'*262144):
                    raise RuntimeError('finite stdout/stderr pressure payload incomplete')
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
                samples.append(dict(round=index, measured=measured, mode='service', signal=number,
                                    returncode=proc.returncode, held_seconds=hold,
                                    cancellation_return_seconds=elapsed,
                                    child_gone=True, locks_released=True,
                                    resources=current, stdout_bytes=len(out), stderr_bytes=len(err)))
            finally:
                cleanup(proc, child_fd)
            settled = resources(os.getpid())
            if settled['fds'] != before_fds:
                raise RuntimeError('controller descriptor growth between rounds')
            samples[-1]['controller_after_cleanup'] = settled
            try:
                os.waitpid(-1, os.WNOHANG)
            except ChildProcessError:
                pass
            else:
                raise RuntimeError('controller retained an unwaited child between rounds')
            index += 1
            if index % 20 == 0:
                elapsed = time.monotonic()-measured_start if measured_start is not None else 0
                print(f'Completed {index} rounds; measured elapsed {elapsed:.1f}s', flush=True)
        measured_elapsed = time.monotonic()-measured_start
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
                  rounds=index, warmup_rounds=args.warmup_rounds,
                  requested_duration_seconds=args.duration_seconds,
                  measured_elapsed_seconds=measured_elapsed,
                  started_at=started_at, measured_started_at=measured_started_at,
                  pressure=args.pressure, samples=samples, controller_fds=[before_fds, after_fds],
                  platform=platform.platform(), python=platform.python_version(),
                  controller_affinity=sorted(os.sched_getaffinity(0)),
                  fixture_sha256=hashlib.sha256(source.encode()).hexdigest(),
                  scope='Controlled real compiled Goose service via Honk build/run; direct signals only. Current procfs RSS/FD snapshots, fresh Honk/service each round; controller retains raw samples. Not universal reliability, continuous Honk uptime, terminal-launch classification, or proof of absence of leaks.')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+'\n')
    print(f'Passed {len(samples)} checked runs; report {args.output}')

if __name__ == '__main__':
    main()
