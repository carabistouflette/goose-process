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
    'pipeline_copy': ('var pipe = process::Pipeline { .. }; var other: process::Pipeline = pipe;', 'root owners cannot be copied'),
    'pipeline_array': ('var pipes: process::Pipeline[1];', 'root'),
    'pipeline_view': ('''var pipe = process::Pipeline { .. };
    let started = process::start_pipeline(["/bin/true"], process::Options { .. },
        ["/bin/cat"], process::Options { .. }, pipe);
    let view = process::pipeline_stdout(pipe);
    let finished = process::close_pipeline(pipe);
    print(view, started.error, finished.error);''', 'shrinks'),
    'copy': ('var other: process::Child = child;', 'root owners cannot be copied'),
    'array': ('var children: process::Child[1];', 'root'),
    'view': ('''let started = process::start(["/bin/true"], process::Options { .. }, child);
    let view = process::stdout(child);
    let finished = process::wait(child);
    print(view, started.error, finished.error);''', 'shrinks'),
    'stdin_view': ('''let started = process::start(["/bin/cat"], process::Options { stdin: process::Input.Stream }, child);
    let view = process::stdout(child);
    var empty: u8[0] = [];
    let progress = process::io_step(child, "", empty[..], empty[..]);
    print(view, started.error, progress.status.error);''', 'shrinks'),
}
for name, (body, diagnostic) in CASES.items():
    path = ROOT / '.build' / f'reject-{name}.goose'
    path.write_text(HEADER + body + '\n}\n')
    result = subprocess.run([sys.argv[1], 'check', str(path)], capture_output=True, text=True, timeout=30)
    assert result.returncode != 0, f'{name}: unsafe ownership unexpectedly accepted'
    assert diagnostic in result.stderr, result.stderr
print(f'{len(CASES)} resource ownership rejection cases passed')
