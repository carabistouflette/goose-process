#!/usr/bin/env python3
"""Seed deterministic native-validation fuzzing without executing commands."""
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path

root = Path(__file__).resolve().parent.parent
corpus = root / '.build/fuzz-corpus'
corpus.mkdir(parents=True, exist_ok=True)
fixtures = [b'/bin/true\0', b'/bin/true\0\0', b'/bin/true\0a b\0',
            b'/bin/true\0' + b'x' * 16384 + b'\0', b'/\0' + b'\0' * 255,
            b'/bin/true\0' + (b'x' * 16384 + b'\0') * 4, b'no-slash\0', b'']
for i, body in enumerate(fixtures):
    (corpus / f'seed-{i}').write_bytes(bytes([64, 6, 1, 1, 0]) + body)
(corpus / 'with-cwd').write_bytes(bytes([64, 6, 1, 1, 4]) + b'/tmp/bin/true\0')
