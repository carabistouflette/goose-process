# Two-stage pipeline API route comparison

This short local experiment compares `run_pipeline` with an explicitly owned
`Pipeline` using the same implementation, commands and successful finite-input
policy. It does not compare another process library or establish isolated copy
cost, general throughput, safety superiority, peak memory or absence of leaks.

## Reproduction and scope

Commit measurement sources first, then run on Linux with the pinned compiler:

```sh
make setup
python3 bench/pipeline_measure.py --goose .toolchain/goose/build/goose --cc gcc --probe-only
taskset -c 0 python3 bench/pipeline_measure.py --goose .toolchain/goose/build/goose --cc gcc --output .build/pipeline-measure.json
```

`--build-only` compiles without invoking the driver. `--probe-only` checks four
unmeasured cells and permits dirty sources. Actual timing refuses dirty or hidden
source changes. CI runs the unmeasured protocol check with GCC and Clang.

Both routes launch the same C producer and consumer through the managed pipe,
use finite Bytes input, the same 30-second shared budget and independent capture
caps, and verify every byte, both successful exits, capture EOF and no ownership
transfer. Tiny input is `00 ff 01`; pressure input is 262,144 bytes with byte
`i % 256`, preceded by 65,536 `x` bytes. Pressure stderr is 65,536 `y` bytes from
the producer and 65,536 `z` bytes from the consumer. Expected capture sizes are
respectively `3/0/0` and `327680/65536/65536`.

Each driver process makes one call with a fresh owner; capacities are not reused.
The driver timer includes owner/buffer initialization and destruction, native
preparation, launch, finite-input copying, capture allocation, pumping, byte/status
validation and cleanup. Convenience capture copying is included. The owner route
also performs a terminal zero-slice step and explicit close. These are complete
API routes, rather than a controlled isolation of the copy operation. Input,
argv/options construction, driver startup and build are outside this timer.
Separately labeled auxiliary process CPU/wall measurements include startup and
must not be substituted for the driver timer.

Nine rounds shuffle the four route/case cells. Each cell runs one separate
warmup process, followed by five measured processes: 180 measured calls and 36
warmups. There is no filtering or retry. Raw ordering, stdout/stderr/status,
watchdog failures, adopted-child recovery and pre/post source/tool/binary hashes
are retained. The controller is a test subreaper; any recovered child fails the
experiment rather than silently satisfying library cleanup assertions.

## Retained result, 2026-11-05

[Raw report](results/pipeline-2026-11-05.json), SHA-256
`f95c3ad3925886e932d0700b22e6f1b717535079de265268e99e83a53d1f94e5`,
was measured from clean source `396ada2b8d9a55a22d405a77833f4a03e6aa889d`.
Native fingerprint was
`eaeda39279a66c4eec9be905cd6c4c7cecf20adf61db40c2e7aeebe2fab0e7e1`.
All 216 calls passed; source, compiler, driver and helper identities were
unchanged. Controller descriptors were 4 before/after and no adopted children
were recovered. These observations concern the controller and this short run;
they are not library peak-memory or sustained resource evidence.

For each cell, take the median of five calls per round, then the median of the
nine round medians. Parentheses give their full range. Units are milliseconds
per complete route; all individual timings remain in the report.

| Case | Convenience copies | Owned borrowed captures |
| --- | ---: | ---: |
| Tiny binary | 1.898 (1.798–1.976) | 1.937 (1.757–2.008) |
| Pressure | 4.502 (4.328–4.683) | 3.986 (3.748–4.053) |

Paired by round, median owner-minus-convenience differences were +0.000044 ms
for tiny input (range −0.141 to +0.190) and −0.620 ms for pressure (−0.800 to
−0.371). Tiny results do not distinguish the routes reliably. The pressure
result supports considering borrowed captures for this workload, while the
unequal terminal operations and shared implementation prevent attributing the
whole difference to copying or claiming an independent engine speedup.

The entire build/provenance/call series ran 19:01:10–19:01:12 UTC, pinned to CPU0
of an Intel Core Ultra 7 258V on Linux 7.2.11/glibc 2.44, GCC 16.2.1,
Python 3.14.9 and `goose eadca40 (runtime 3025e5810379)` from the repository pin.
Affinity is not isolation; this was a shared host with load averages retained.
No exclusive-core reservation, independent repeat, sustained run, cancellation
timing, cross-language policy parity or universal performance ranking is claimed.
