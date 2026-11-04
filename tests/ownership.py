#!/usr/bin/env python3
"""Check resource ownership and capture-view lifetimes at compile time."""
# SPDX-License-Identifier: Apache-2.0
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent
HEADER = '''import process from "..";
import std;
fn main() {
    var child = process::Child { .. };
'''
CASES = {
    'copy': ('var other: process::Child = child;', 'root owners cannot be copied'),
    'array': ('var children: process::Child[1];', 'root'),
    'view': ('''let started = process::start(["/bin/true"], process::Options { .. }, child);
    let view = process::stdout(child);
    let finished = process::wait(child);
    print(view, started.error, finished.error);''', 'shrinks'),
}
for name, (body, diagnostic) in CASES.items():
    path = ROOT / '.build' / f'reject-{name}.goose'
    path.write_text(HEADER + body + '\n}\n')
    result = subprocess.run([sys.argv[1], 'check', str(path)], capture_output=True, text=True, timeout=30)
    assert result.returncode != 0, f'{name}: unsafe ownership unexpectedly accepted'
    assert diagnostic in result.stderr, result.stderr
print('3 resource ownership rejection cases passed')
