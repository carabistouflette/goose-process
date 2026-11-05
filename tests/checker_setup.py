#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Ensure optional test-tool setup never accepts hidden or modified source."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT/'scripts'))
import checker_consumer as consumer


def rejected(path, revision):
    try:
        consumer.clean_source(path, revision)
    except ValueError:
        return
    raise AssertionError('unsafe checker source accepted')


with tempfile.TemporaryDirectory(prefix='checker source probe ', dir=ROOT/'.build') as temp:
    target = Path(temp)/'source'
    subprocess.run(['git','clone','--no-local',str(consumer.SOURCE),str(target)],check=True)
    consumer.clean_source(target, consumer.REVISION)
    rejected(target, '0'*40)
    original = (target/'README.md').read_bytes()
    (target/'README.md').write_bytes(original+b'\nmodified\n')
    rejected(target, consumer.REVISION)
    subprocess.run(['git','update-index','--assume-unchanged','README.md'],cwd=target,check=True)
    rejected(target, consumer.REVISION)
    link=Path(temp)/'linked'; link.symlink_to(target, target_is_directory=True)
    rejected(link, consumer.REVISION)
    nested=target/'.build'; nested.mkdir(exist_ok=True)
    rejected(nested, consumer.REVISION)
print('Checker source pin, dirtiness, hidden-index, symlink and checkout-boundary probes passed')
