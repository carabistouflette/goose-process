#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Check fixed adapters' byte oracles and cleanup after wrong worker behavior."""
import argparse
import os
from pathlib import Path
import subprocess
import sys

import measure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--goose', default=str(measure.ROOT / '.toolchain/goose/build/goose'))
    parser.add_argument('--cc', default='gcc')
    args = parser.parse_args()
    measure.controller.controller_scope()
    try:
        worker, commands, _, _ = measure.build(args)
        directory = worker.parent
        measure.controller.command([measure.controller.executable(args.cc), '-std=c11', '-Wall', '-Wextra',
                                    '-Werror', '-O2', measure.HERE / 'fault.c', '-o', directory / 'fault'])
        for name in ('early', 'corrupt', 'excess'):
            link = directory / name
            link.unlink(missing_ok=True)
            os.link(directory / 'fault', link)
        for command in commands.values():
            for size in (31, 4096):
                measure.checked(command, worker, size)
            for name in ('early', 'corrupt', 'excess'):
                code, _, _ = measure.controller.run_owned([*command, directory / name, '31', '128'],
                                                          stderr=subprocess.PIPE)
                adopted = measure.controller.adopted_cleanup()
                if code == 0 or adopted:
                    raise RuntimeError(f'failed oracle or cleanup for {name}: status{code}, adopted{adopted}')
        print('8 successful and12 rejected duplex adapter cases passed; no adopted children')
    finally:
        if measure.controller.adopted_cleanup():
            raise RuntimeError('final adapter cleanup found a child')


if __name__ == '__main__':
    main()
