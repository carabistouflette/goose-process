# Historical incremental-input checks and measurements

These results describe the revisions named below, not current-main performance.
See [benchmark source scope](../bench/CURRENTNESS.md).

## Retained bulk-workload timing

```sh
# Commit benchmark source first; setup/build is outside timing.
taskset -c 0 python3 bench/stdin_measure.py --goose .toolchain/goose/build/goose --cc gcc
```

[Raw samples](../bench/results/stdin-2026-11-05.json) were measured from clean
source `e6286440045a6ff30354c30d6a3c3f8e216b0c8c`. All 90 timed batches and
18 separate warmups passed. Source/compiler/binary identities remained unchanged,
and the controller retained four descriptors before/after with no adopted child.

Each batch supplies the same 128 frames: 524,800 input/stdout bytes and 512
stderr acknowledgment bytes. Both routes capture outputs with a 1 MiB per-stream
cap and a 30-second process budget. Finite uses synchronous run with copied
Bytes; incremental uses caller-driven io_step with explicit EOF and no queued
input copy. Total-input policy and control granularity differ, so this is not
complete safety-policy parity. Batch construction, build and driver startup are
outside the timer; public calls, draining/reaping, comparison and incremental
close are inside it. Buffers/Child are reused.

| Route | Median of nine round medians, ms | Full round-median range, ms |
| --- | ---: | ---: |
| Finite Bytes | 2.193 | 2.135–2.338 |
| Incremental Stream | 2.120 | 2.073–2.281 |

Each round median covers five calls, with shuffled route order and one separate
warmup per cell process. Ranges overlap; the small difference in this single
short CPU0 series does not establish a general speedup. Shared-host load,
toolchain/CPU/flags and all raw values are retained. This is neither interactive
request latency, a memory/allocation measurement nor a cross-language comparison.
The useful new capability is bounded caller-driven input, not a timing promise.

## Validation and publication scope

Three independent contract, native security/lifecycle and acceptance/provenance
reviews found no concrete defects in their reviewed scope. Normal interpreter,
optimized/debug public tests and worker clients passed, including eight actual
worker protocol boundary/error cases. The full native/model/validation suite,
GCC and Clang ASan/UBSan suites, Clang static analysis of both library C sources
and the worker, and the intercepted-spawn 10,000-run fuzzer passed locally.
The latest accepted-count/transfer/EAGAIN fixtures were additionally checked
under both sanitizer compilers; the latest stdin suite passed optimized/debug
execution. The pinned-checker integration passed with the worker's exact stdout.

A clean local clone through feature `b6edad1b4c36661b4fe866c7e3042aab5e272904`,
with spaces in its path and a fresh `GOOSE_HOME`, passed checker setup/probes,
the actual worker consumer and debug incremental-input tests, using the matching
existing compiler. This is not a new compiler bootstrap or anonymous public-clone
execution claim. Subsequent changes add retry-coverage robustness and docs;
library runtime source and the measured driver remain unchanged since the named
measurement source. Hosted CI conclusions are a separate exact-head gate.

This milestone changes experimental runtime/native code. It does not move any
release tag or silently repin Honk, goose-check or the parent repository. Existing
Capture and finite Bytes semantics stay in place. There is no claim of universal
feature parity, speed, memory use or absence of bugs.
