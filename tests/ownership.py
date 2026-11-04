#!/usr/bin/env python3
"""Require compiler rejection of input/output lifetime aliasing."""
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
HEADER = '''import process from "..";
import std;
fn main() {
    var out = str("/bin/true");
    var err: u8[>..] = [];
    let view: const u8[:] = out[..];
'''
CASES = {
    'args': 'let s = process::run([view], process::Options { .. }, out, err);',
    'cwd': 'let s = process::run(["/bin/true"], process::Options { cwd: view }, out, err);',
    'stderr': 'let s = process::run([view], process::Options { .. }, err, out);',
}
for name, call in CASES.items():
    path = ROOT / '.build' / f'reject-{name}.goose'
    path.write_text(HEADER + call + '\n print(s.error);\n}\n')
    result = subprocess.run([sys.argv[1], 'check', str(path)], capture_output=True, text=True, timeout=30)
    assert result.returncode != 0, f'{name}: aliasing unexpectedly accepted'
    assert '§5.1' in result.stderr and ('shrinks' in result.stderr or 'cannot clear' in result.stderr), result.stderr
print('3 ownership rejection cases passed')
