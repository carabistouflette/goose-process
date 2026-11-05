#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check the actual bounded framed worker, including truncated/oversized input."""
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'scripts'))
import checker_consumer as controller


def main():
    worker = Path(sys.argv[1]).resolve()
    controller.controller_scope()
    before = len(list(Path('/proc/self/fd').iterdir()))
    empty = b'\x00' * 4
    maximum = b'\x00\x00\x10\x00' + bytes(range(256)) * 16
    cases = [
        (b'', 0, b'', b''),
        (empty, 0, empty, b'ack\x00'),
        (empty + maximum, 0, empty + maximum, b'ack\x00' * 2),
        (b'\x00' * 3, 2, b'', b''),
        (b'\x00\x00\x00\x03x', 2, b'', b''),
        (b'\x00\x00\x10\x01', 2, b'', b''),
        (b'\xff' * 4, 2, b'', b''),
        (empty + b'\x00' * 3, 2, empty, b'ack\x00'),
    ]
    for payload, status, expected_out, expected_err in cases:
        try:
            result, output, errors = controller.run_owned([worker], input_bytes=payload, timeout=5,
                                                           stderr=subprocess.PIPE)
            if (result, output, errors) != (status, expected_out, expected_err):
                raise AssertionError(('worker protocol differs', result, output, errors))
        finally:
            if controller.adopted_cleanup():
                raise AssertionError('worker retained an adopted child')
    if len(list(Path('/proc/self/fd').iterdir())) != before:
        raise AssertionError('worker controller descriptor growth')
    print('8 actual worker framing/binary/boundary/error cases passed')


if __name__ == '__main__':
    main()
