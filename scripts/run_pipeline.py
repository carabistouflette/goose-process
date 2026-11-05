#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Bound a pipeline fixture and recover owned orphan fixtures after failure."""
import subprocess
import sys

import checker_consumer as controller


def main():
    controller.controller_scope()
    output = errors = b''
    code = 1
    try:
        code, output, errors = controller.run_owned(sys.argv[1:], timeout=60, stderr=subprocess.PIPE)
    except subprocess.TimeoutExpired as error:
        output, errors = error.output or b'', error.stderr or b''
        raise
    finally:
        adopted = controller.adopted_cleanup()
        sys.stdout.buffer.write(output); sys.stdout.buffer.flush()
        sys.stderr.buffer.write(errors); sys.stderr.buffer.flush()
        if adopted:
            raise RuntimeError(f'pipeline fixture abandoned {adopted} owned children; recovered')
    raise SystemExit(code)


if __name__ == '__main__':
    main()
