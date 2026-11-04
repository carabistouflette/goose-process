#!/usr/bin/env python3
"""Compare native validation with an independent bounded input model; no exec."""
# SPDX-License-Identifier: Apache-2.0
import ctypes as c
import errno
import os
from pathlib import Path
import random

ROOT = Path(__file__).resolve().parent.parent

class Slice(c.Structure):
    _fields_ = [('data', c.c_void_p), ('length', c.c_int64)]

def valid(packed, cwd, timeout, cap, capture):
    if not (2 <= len(packed) <= 65536 and packed.startswith(b'/') and packed.endswith(b'\0')):
        return False
    if len(cwd) > 4095 or b'\0' in cwd or not 1 <= timeout <= 86400000:
        return False
    if not 0 <= cap <= 1048576 or capture not in (0, 1):
        return False
    arguments = packed[:-1].split(b'\0')
    return (len(arguments) <= 256 and len(arguments[0]) <= 4095
            and all(len(argument) <= 16384 for argument in arguments))

def fixtures():
    def case(args, cwd=b'', timeout=1000, cap=1, capture=1):
        return b'\0'.join(args) + b'\0', cwd, timeout, cap, capture
    for size in [0, 1, 2, 4094, 4095, 4096]:
        yield case([b'/' + b'x' * (size - 1)] if size else [b''])
    for size in [0, 1, 16383, 16384, 16385]:
        yield case([b'/x', b'x' * size])
    for count in [1, 255, 256, 257]:
        yield case([b'/x'] + [b''] * (count - 1))
    for size in [4094, 4095, 4096]:
        yield case([b'/x'], b'x' * size)
    for size in [0, 1, 65535, 65536, 65537]:
        if size < 3:
            yield b'/'[:size], b'', 1000, 1, 1
            continue
        args = [b'/x']
        remaining = size - 3
        while remaining > 0:
            take = min(16385, remaining)
            args.append(b'x' * (take - 1))
            remaining -= take
        yield case(args)
    for timeout in [0, 1, 86400000, 86400001]:
        yield case([b'/x'], timeout=timeout)
    for cap in [-1, 0, 1, 1048576, 1048577]:
        yield case([b'/x'], cap=cap)
    for capture in [-1, 0, 1, 2]:
        yield case([b'/x'], capture=capture)
    rng = random.Random(91107)
    for _ in range(2500):
        count = rng.choice([0, 1, 2, 255, 256, 257])
        args = [b'/x'] if count else []
        remaining = 65537 - 3
        for _ in range(max(count - 1, 0)):
            size = min(rng.choice([0, 1, 2, 128, 1024, 16384, 16385]), max(remaining - 1, 0))
            args.append(bytes([rng.randrange(256)]) * size)
            remaining -= size + 1
        # Stream cases instead of retaining thousands of large input buffers.
        yield case(args, cwd=rng.choice([b'', b'/tmp', b'a\0b']))

def main():
    lib = c.CDLL(str(ROOT / '.build/validation.so'))
    lib.gp_run.argtypes = [Slice, Slice, c.c_int64, c.c_int64, Slice, Slice, Slice]
    lib.gp_run.restype = c.c_int64
    before = len(os.listdir('/proc/self/fd'))
    checked = 0
    for packed, cwd, timeout, cap, capture in fixtures():
        expected = 2 if valid(packed, cwd, timeout, cap, capture) else 1
        source = c.create_string_buffer(packed)
        directory = c.create_string_buffer(cwd)
        capacity = max(cap, 0)
        out = c.create_string_buffer(b'\xa5' * (capacity + 16), capacity + 16)
        err = c.create_string_buffer(b'\x5a' * (capacity + 16), capacity + 16)
        metadata = (c.c_int64 * 8)(123, 0, 0, 0, 0, 0, 0, 456)
        result = lib.gp_run(Slice(c.addressof(source), len(packed)),
            Slice(c.addressof(directory), len(cwd)), timeout, capture,
            Slice(c.addressof(out), cap), Slice(c.addressof(err), cap),
            Slice(c.addressof(metadata) + c.sizeof(c.c_int64), 6))
        assert result == expected, (result, expected, len(packed), len(cwd), timeout, cap, capture)
        assert list(metadata) == [123, errno.EACCES if expected == 2 else 0, -1, 0, 0, 0, 0, 456]
        assert out.raw == b'\xa5' * (capacity + 16) and err.raw == b'\x5a' * (capacity + 16)
        assert len(os.listdir('/proc/self/fd')) == before
        checked += 1
    print(f'{checked} independent native validation classifications passed (execution refused)')

if __name__ == '__main__':
    main()
