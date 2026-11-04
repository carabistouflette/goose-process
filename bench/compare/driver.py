#!/usr/bin/env python3
"""Python standard-library baseline; successful workloads, uncapped communicate."""
# SPDX-License-Identifier: Apache-2.0
import subprocess
import sys
import time

fixture, mode, count_text = sys.argv[1:]
count = int(count_text)
command = [fixture, mode] + (['x' * 32] * 128 if mode == 'args' else [])
input_data = b'x' * 131072 if mode == 'input' else None
expected = b'x' * (131072 if mode == 'input' else 65536) if mode in ('input', 'streams', 'redirect') else b''
errors = b'y' * 65536 if mode == 'streams' else b''
options = dict(stdin=subprocess.PIPE if input_data is not None else subprocess.DEVNULL,
               stdout=subprocess.PIPE,
               stderr=subprocess.DEVNULL if mode == 'redirect' else subprocess.PIPE,
               env={'PROBE': 'goose'} if mode == 'environment' else None)

def invoke():
    with subprocess.Popen(command, **options) as child:
        out, err = child.communicate(input_data)
        if child.returncode != 0 or out != expected or (err or b'') != errors:
            raise RuntimeError('child failed or output mismatch')

for _ in range(3):
    invoke()
start = time.perf_counter_ns()
for _ in range(count):
    invoke()
print((time.perf_counter_ns() - start) // count)
