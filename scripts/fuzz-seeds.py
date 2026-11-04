#!/usr/bin/env python3
"""Seed deterministic native-validation fuzzing without executing commands."""
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path

root = Path(__file__).resolve().parent.parent
corpus = root / '.build/fuzz-corpus'
corpus.mkdir(parents=True, exist_ok=True)
fixtures = [b'/bin/true\0', b'/bin/true\0\0', b'/bin/true\0a b\0',
            b'/bin/true\0' + b'x' * 131071 + b'\0', b'/\0' + b'\0' * 4095,
            b'/bin/true\0' + (b'x' * 131071 + b'\0') * 8, b'no-slash\0', b'']
for i, body in enumerate(fixtures):
    (corpus / f'seed-{i}').write_bytes(bytes([64, 6, 1, 1, 0]) + body)
(corpus / 'with-cwd').write_bytes(bytes([64, 6, 1, 1, 4]) + b'/tmp/bin/true\0')
for name, sets, removes in [('env', b'KEY=value\0', b'OLD\0'),
                            ('duplicate', b'KEY=value\0', b'KEY\0')]:
    (corpus / name).write_bytes(bytes([64, 6, 1, 16, 0, len(sets), len(removes), 0, 0])
                               + sets + removes + b'true\0')
