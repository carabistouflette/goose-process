#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Keep pre-measurement rejection diagnostics without launching a fixture."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('duplex_pair', ROOT / 'bench/duplex_pair.py')
pair = importlib.util.module_from_spec(spec); spec.loader.exec_module(pair)


def rejected(directory, before, after, kind):
    output = directory / 'failure.json'
    arguments = ['duplex_pair.py', '--before', str(before), '--after', str(after),
                 '--before-directory', str(directory), '--after-directory', str(directory), '--output', str(output)]
    original_git = pair.shared.controller.git
    def git(*args, **kwargs):
        # Rejection probes must also work in an ordinary dirty development tree.
        return '' if args == ('status', '--porcelain') else original_git(*args, **kwargs)
    with patch.object(sys, 'argv', arguments), patch.object(pair.shared.controller, 'git', git):
        try:
            pair.main()
        except (FileNotFoundError, ValueError):
            pass
        else:
            raise AssertionError('invalid paired inputs were accepted')
    report = json.loads(output.read_text())
    if report['status'] != 'failed' or report['error_kind'] != kind or report['samples']:
        raise AssertionError('startup rejection lost its status or launched a sample')


def main():
    (ROOT / '.build').mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(dir=ROOT / '.build') as temp:
        directory = Path(temp)
        rejected(directory, directory / 'missing.json', directory / 'missing-after.json', 'FileNotFoundError')
        names = dict(binary_goose='goose', binary_rust='rust', worker='worker', rust_native_process='process.o',
                     rust_native_launch='launch.o', native_archive='libduplexshared.a')
        hashes = {}
        for name, file in names.items():
            (directory / file).write_bytes(b'nonexecutable fixture')
            hashes[name] = hashlib.sha256(b'nonexecutable fixture').hexdigest()
        hashes['binary_goose'] = '0' * 64
        record = dict(status='complete', hashes=hashes)
        before = directory / 'record.json'; before.write_text(json.dumps(record))
        rejected(directory, before, before, 'ValueError')
    print('Paired startup missing-record and corrupt-artifact reports retained without samples')


if __name__ == '__main__':
    main()
