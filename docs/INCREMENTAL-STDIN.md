# Incremental stdin and a framed worker

The [complete interface contract](API.md#bounded-incremental-stdin) is
authoritative. Input.Stream is explicit and start-only. Existing finite Bytes,
inherited, null and borrowed-FD input semantics remain unchanged.

```goose
let launched = process::start(command, process::Options {
    stdout: process::Output.Stream, stderr: process::Output.Stream,
    stdin: process::Input.Stream,
}, child);
// Check launched, then offer unaccepted suffixes while consuming both outputs.
let progress = process::io_step(child, offered, out, err, 20);
// Account for input_accepted and output prefixes even on terminal errors.
// When all wanted input is accepted, explicitly close stdin and continue pumping.
```

There is no library payload queue. Each call borrows at most 1 MiB, accepts at
most 16 KiB, and retains no input pointer. Caller buffers remain stable during
the call and can be reused afterward. The caller must keep its unaccepted suffix
and inspect input_open, done and status. An empty offer does not imply EOF.
Closing the write endpoint sends EOF after kernel-accepted bytes drain; it does
not acknowledge consumption or wait for the worker. See CALLERS.md for explicit
exceptional cleanup ownership rather than treating this sketch as a full runner.

## Run the worker demonstration

```sh
make setup helpers
.toolchain/goose/build/goose build examples/worker.goose --cc gcc -O2 -o .build/worker-client
.build/worker-client
```

`examples/worker.c` is a fixed framed echo worker: four-byte big-endian length,
at most 4096 payload bytes; stdout echoes the frame, stderr emits `ack\0`.
Truncated/oversized frames fail with a nonzero exit. It is not an executable
command protocol or general worker framework. The Goose client sends 400
variable-size frames (including empty and binary payloads), validates every
response and acknowledgment with fixed buffers, then explicitly closes stdin
and reaps the worker. The cumulative input/stdout is 1,149,760 bytes, exceeding
the finite Bytes limit, with no whole-log/input queue in the client. It prints
only after normal cleanup; its assertions are fixed-fixture test oracles, not a
complete production recovery policy for hostile workers or transferred PIDs.

Normal tests run optimized/debug versions. The pinned external goose-check
consumer also validates the client's exact stdout; that checker retains its
existing older process pin, while the client links this checkout. No application
or parent dependency pin is implicitly updated.

## Regression coverage

`tests/stdin.goose` verifies a 3 MiB binary roundtrip through changing remaining
views, >socket-capacity duplex input while the child fills both outputs before
reading, empty offers, explicit/repeated EOF, mid-process peer closure, discarded
unaccepted bytes, capture-only/mixed routing and overflow, invalid offers/slices,
reaping before output drain, terminal repetition and owner reuse. Legacy pumping
methods reject new-mode owners; synchronous run rejects before launch.

`tests/stdin.c` injects a clock failure immediately after a real successful send:
accepted bytes remain reported, cleanup is terminal, waitpid proves ECHILD and
descriptors return to baseline. A targeted denied terminal wait also transfers
the PID exactly once while preserving accepted bytes; the local recovery owner
then proves terminal reaping/ECHILD. A retryable EAGAIN reports zero acceptance,
and a following empty offer proves no pending payload was retained. It also
checks result/input/output/state aliases and input poll events=0 for an empty offer.
MSG_NOSIGNAL is asserted at
the send seam. Existing finite-input signal-state, EINTR deadlines, lifecycle and
denied-cleanup transfer tests remain active. The intercepted-spawn fuzzer now
exercises start-only input routing without executing mutated paths.

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
