# Streaming validation and local measurements

The [interface contract](API.md#bounded-live-output) is authoritative. Run
`make setup check test` to build the pinned compiler and execute the public
interface regressions, including optimized/debug streaming tests and a compiler
consumer. `make checker-setup checker-source-test checker-consumer CC=gcc`
(serial) also checks the compiler consumer's stdout with the pinned external
fixture tool. That tool retains its older library pin; test executables link this
checkout. No application or dependency pin has been silently updated.

`examples/compiler.goose` feeds valid and invalid C source to `/usr/bin/cc` and
counts diagnostic bytes/newlines as bounded chunks arrive, with no retained log.
It prints its result after compiler cleanup. This is a fixed-fixture demonstration,
not a general production runner or complete exceptional transferred-PID recovery
implementation; applications need the [caller obligations](CALLERS.md).

## Acceptance coverage

`tests/stream.goose` checks two 3 MiB streams through unequal small buffers,
one-byte delivery, empty-span backpressure, mixed captures and overflow, merged
stderr ordering, 1 MiB finite binary stdin, reaping before drain, terminal
observation, invalid/legacy operations and owner reuse. Byte oracles remain
binary and length-exact. Stream does not accumulate whole output in its owner.

`tests/stream.c` adds actual read-failure and timeout cleanup, alias rejection,
direct-child ECHILD proof, descriptor counts and a held-descendant-EOF fixture.
The fixture controller adopts/reaps its own descendant after library completion;
this does not add process-tree supervision to the library. Existing lifecycle,
denied-cleanup transfer, signal-state, descriptor and intercepted-spawn fuzz
regressions continue to exercise the shared machinery.

Independent contract, native security/lifecycle, and acceptance/provenance
reviews found no actionable defects in their reviewed streaming scope. Local
and hosted execution results are recorded separately; a review is not a test run.

## Reproducible healthy-output timing

Commit source first, then run:

```sh
taskset -c 0 python3 bench/stream_measure.py --goose .toolchain/goose/build/goose --cc gcc
```

The [retained raw report](../bench/results/stream-2026-11-05.json) identifies
clean source `072d125f963104437038153bcb42bbc521cec28a`, compiler and binary
hashes, flags, CPU affinity/model, host load and versions. Nine shuffled rounds
contain six cells, with one warmup and five timed calls per cell: 54 warmups and
270 timed calls, all successful. Build/driver startup is outside the reported
public-call timer. Timers include launch, reading/reaping and byte validation.
Capture buffers and streamed Child are reused; a fresh-process measurement would
have different allocation/startup costs.

Each successful command emits identical bytes/status: 64 KiB or 1 MiB **on each**
of stdout and stderr. Capture enforces a total cap; Stream has no total cap.
These are unequal safety policies, not a matched-contract or language comparison.
No additional optimization was made to obtain these results.

Milliseconds below are the median of the nine round medians, each round taking
the median of its five timed calls. Parentheses show the complete round-median
range; individual samples and every warmup remain in the report.

| Bytes per stream | Capture | Stream 4 KiB view | Stream 16 KiB view |
| --- | ---: | ---: | ---: |
| 64 KiB | 0.817 (0.773–0.873) | 0.799 (0.770–0.986) | 0.822 (0.752–0.891) |
| 1 MiB | 3.212 (2.966–3.691) | 3.421 (3.135–3.618) | 2.919 (2.698–3.228) |

Small-output differences overlap shared-host noise, and the 4 KiB view regresses
on the larger payload. The 16 KiB result is specific to this short run. Retain
both outcomes; neither proves a general speedup. Both streaming routes use fixed
16 KiB stack storage per output; the 4 KiB route exposes only a 4 KiB view.
This measures neither peak memory nor allocations: bounded-storage claims come
from the interface/implementation, not RSS inference. No exclusive-core claim,
independent repeat, tail-latency estimate or sustained-throughput claim is made.
