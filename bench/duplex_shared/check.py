#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate fixed shared-kernel wrappers against successful and bad workers."""
import argparse
import os
from pathlib import Path
import subprocess

import measure


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--goose', default=str(measure.ROOT / '.toolchain/goose/build/goose'))
    parser.add_argument('--cc', default='gcc')
    parser.add_argument('--directory', type=Path, default=measure.ROOT / '.build/duplex-shared')
    args = parser.parse_args()
    measure.controller.controller_scope()
    try:
        worker, commands, identities, _ = measure.build(args)
        before = {name: measure.digest(path) for name, path in identities.items()}
        directory = worker.parent
        measure.controller.command([measure.controller.executable(args.cc), '-std=c11', '-Wall', '-Wextra',
                                    '-Werror', '-O2', measure.ROOT / 'bench/duplex_compare/fault.c',
                                    '-o', directory / 'fault'])
        for name in ['early', 'corrupt', 'excess', 'frontload']:
            link = directory / name; link.unlink(missing_ok=True); os.link(directory / 'fault', link)
        for command in commands.values():
            for size in [31, 4096]:
                measure.base.checked(command, worker, size)
            for name in ['early', 'corrupt', 'excess', 'frontload']:
                code, _, _ = measure.controller.run_owned([*command, directory / name, '31', '128'],
                                                          stderr=subprocess.PIPE)
                adopted = measure.controller.adopted_cleanup()
                if code == 0 or adopted:
                    raise RuntimeError(f'bad-worker oracle/cleanup failed for {name}: status{code}, adopted{adopted}')
        if before != {name: measure.digest(path) for name, path in identities.items()}:
            raise RuntimeError('checked wrapper/object identity changed')
        print('4 successful and8 rejected shared-native-object duplex cases passed; no adopted children')
    finally:
        if measure.controller.adopted_cleanup():
            raise RuntimeError('final shared-kernel cleanup found a child')


if __name__ == '__main__':
    main()
