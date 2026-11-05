#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fixed bounded protocol benchmark, not a general subprocess adapter."""
import os
import selectors
import subprocess
import sys
import time


def main():
    if len(sys.argv) != 4 or not os.path.isabs(sys.argv[1]):
        raise ValueError('absolute worker, payload size and request count required')
    size, requests = map(int, sys.argv[2:])
    if size not in (31, 4096) or requests != 128:
        raise ValueError('unsupported fixed workload')
    frame = size.to_bytes(4, 'big') + bytes((i * 17 + i // 251) % 256 for i in range(size))
    ack = b'ack\0'
    began = time.monotonic_ns()
    deadline = time.monotonic() + 30
    proc = subprocess.Popen([sys.argv[1]], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, close_fds=True)
    selector = None
    streams = {proc.stdout.fileno(): 'out', proc.stderr.fileno(): 'err'}
    try:
        selector = selectors.DefaultSelector()
        for stream in (proc.stdin, proc.stdout, proc.stderr):
            os.set_blocking(stream.fileno(), False)
        for fd, label in streams.items():
            selector.register(fd, selectors.EVENT_READ, label)
        for _ in range(requests):
            sent = received = acknowledged = 0
            selector.register(proc.stdin, selectors.EVENT_WRITE, 'in')
            while sent < len(frame) or received < len(frame) or acknowledged < 4:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError('worker deadline')
                for key, _ in selector.select(min(0.020, remaining)):
                    try:
                        if key.data == 'in':
                            count = os.write(key.fd, frame[sent:])
                            if count <= 0:
                                raise RuntimeError('zero stdin progress')
                            sent += count
                            if sent == len(frame):
                                selector.unregister(proc.stdin)
                        else:
                            chunk = os.read(key.fd, 257 if key.data == 'out' else 3)
                            if not chunk:
                                raise RuntimeError('early worker EOF')
                            if key.data == 'out':
                                if len(chunk) > len(frame) - received or chunk != frame[received:received + len(chunk)]:
                                    raise RuntimeError('stdout oracle')
                                received += len(chunk)
                            else:
                                if len(chunk) > 4 - acknowledged or chunk != ack[acknowledged:acknowledged + len(chunk)]:
                                    raise RuntimeError('stderr oracle')
                                acknowledged += len(chunk)
                    except (BlockingIOError, InterruptedError):
                        pass
        proc.stdin.close()
        while streams:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError('worker EOF deadline')
            for key, _ in selector.select(min(0.020, remaining)):
                try:
                    chunk = os.read(key.fd, 257 if key.data == 'out' else 3)
                except (BlockingIOError, InterruptedError):
                    continue
                if chunk:
                    raise RuntimeError('excess worker output')
                selector.unregister(key.fd)
                streams.pop(key.fd)
        remaining = deadline - time.monotonic()
        if remaining <= 0 or proc.wait(timeout=remaining) != 0:
            raise RuntimeError('worker exit/deadline')
    finally:
        if selector is not None:
            selector.close()
        if proc.returncode is None:
            proc.kill()
            proc.wait(timeout=10)
        for stream in (proc.stdin, proc.stdout, proc.stderr):
            stream.close()
    print(time.monotonic_ns() - began, requests * len(frame), requests * 4)


if __name__ == '__main__':
    main()
