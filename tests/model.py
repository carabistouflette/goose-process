#!/usr/bin/env python3
"""Independent subprocess oracle for byte arguments, bounds and launch errors."""
# SPDX-License-Identifier: Apache-2.0
import ctypes as c
import errno
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent

class Slice(c.Structure):
    _fields_ = [('data', c.c_void_p), ('length', c.c_int64)]

def main():
    lib = c.CDLL(str(ROOT / '.build/process.so'))
    lib.gp_run.argtypes = [Slice, Slice, c.c_int64, c.c_int64, Slice, Slice, Slice]
    lib.gp_run.restype = c.c_int64
    helper = os.fsencode(ROOT / '.build/helper')
    cases = 0

    def run(args, cap=65536, cwd=b'', capture=1):
        nonlocal cases
        packed = b'\0'.join(args) + b'\0'
        inp = c.create_string_buffer(packed)
        directory = c.create_string_buffer(cwd)
        # Canary bytes detect writes beyond the logical output cap.
        out = c.create_string_buffer(b'\xa5' * (cap + 16), cap + 16)
        err = c.create_string_buffer(b'\x5a' * (cap + 16), cap + 16)
        meta = (c.c_int64 * 6)()
        result = lib.gp_run(Slice(c.addressof(inp), len(packed)),
            Slice(c.addressof(directory), len(cwd)), 5000, capture,
            Slice(c.addressof(out), cap), Slice(c.addressof(err), cap),
            Slice(c.addressof(meta), 6))
        assert 0 <= meta[3] <= cap and 0 <= meta[4] <= cap
        assert out.raw[cap:] == b'\xa5' * 16 and err.raw[cap:] == b'\x5a' * 16
        assert meta[5] == 0
        cases += 1
        return result, list(meta), out.raw[:meta[3]], err.raw[:meta[4]]

    if len(sys.argv) > 1 and sys.argv[1] == 'inherit':
        result, meta, out, err = run([helper, b'bytes'], capture=0)
        assert result == 0 and meta[1] == 0 and out == err == b''
        return
    inherited = subprocess.run([sys.executable, __file__, 'inherit'],
        capture_output=True, timeout=10, check=True)
    assert inherited.stdout == bytes(range(256))
    assert inherited.stderr == bytes(reversed(range(256)))

    rng = random.Random(3025)
    fixtures = [[b'', b'a b', b"'\";$(echo bad)\\*?", b'\xff\xfe\n\r']]
    for _ in range(200):
        fixtures.append([bytes(rng.randrange(1, 256) for _ in range(rng.randrange(100)))
                         for _ in range(rng.randrange(15))])
    fixtures += [[b'x' * 16384], [b''] * 254,
                 [b'x' * 16384] * 3 + [b'x' * 10000]]
    for fields in fixtures:
        args = [helper, b'args', *fields]
        expected = subprocess.run(args, capture_output=True, check=True, timeout=5)
        result, meta, out, err = run(args)
        assert result == 0 and meta[1] == expected.returncode == 0
        assert out == expected.stdout and err == expected.stderr
    for cap in [0, 1, 255, 256, 4095, 4096, 16384, 65536]:
        for length in sorted({0, cap, cap + 1}):
            result, meta, out, err = run([helper, b'streams', str(length).encode()], cap)
            assert result == (5 if length > cap else 0)
            assert out == b'x' * min(length, cap)
            # On overflow stdout may trigger cancellation before stderr is drained.
            assert err == b'y' * len(err)
            if length <= cap: assert len(err) == length
        result, meta, out, err = run([helper, b'stderr', str(cap + 1).encode()], cap)
        assert result == 5 and out == b'' and err == b'e' * cap
    for args in [[helper, b'args', b'x' * 16385], [helper, b'args', *([b''] * 255)],
                 [helper, b'args', *([b'x' * 16384] * 4)], [b'true'], [b'']]:
        assert run(args)[0] == 1
    with tempfile.TemporaryDirectory(prefix='goose-process-') as directory:
        path = Path(directory) / 'not-a-program'
        path.write_text('exit 0\n')
        path.chmod(0o700)
        result, meta, _, _ = run([os.fsencode(path)])
        assert result == 2 and meta[0] == errno.ENOEXEC
        path.chmod(0o600)
        result, meta, _, _ = run([os.fsencode(path)])
        assert result == 2 and meta[0] == errno.EACCES
        assert run([helper, b'cwd'], cwd=os.fsencode(directory))[2] == os.fsencode(directory)
    result, meta, _, _ = run([b'/no/such/goose-process-file'])
    assert result == 2 and meta[0] == errno.ENOENT
    print(f'{cases} independent subprocess oracle cases passed')

if __name__ == '__main__':
    main()
