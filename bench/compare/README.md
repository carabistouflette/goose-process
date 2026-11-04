# Rust, Go, Python and Goose process comparison

[Recorded results, toolchains and sample ranges](RESULTS.md) are available with
[all raw samples](../results/linux-stdlib-2026-11-04.json).

Run from the repository root on Linux after `make setup`:

```sh
python3 bench/compare/measure.py --build-only
python3 bench/compare/measure.py --cpu 0 --rounds 9 --iterations 40
```

Requires the pinned Goose compiler, GCC (or `--cc clang`), Rust `rustc`, Go, and
Python 3.11+. No third-party packages are downloaded. The script compiles the
same C child fixture for all drivers, builds optimized Goose/Rust/Go drivers,
and runs Python directly. `--build-only` validates all 24 driver/fixture pairs
without retaining a timing report. Generated executables and default results
stay in ignored `.build/`; `--output PATH` selects a report destination.

This is the **native API, successful-workload track**, not an equal safety-cost
benchmark. All variants use the same absolute child executable, literal argv,
null stdin unless input is supplied, binary captures, environment selection and
expected output/status. Every invocation checks exact bytes and success inside
the timed loop. Three calls warm each driver/fixture before each sample. Driver
startup/imports, fixture compilation and expected-data construction are excluded.
Each measured sample is the average of 40 sequential successful calls. Reported
medians are medians of these batch means, not individual-call latency medians;
nine samples are not enough to characterize individual-call p99 latency.

| Fixture | Child work and checked output |
| --- | --- |
| empty | Exit 0; empty captures |
| args | Verify 128 additional 32-byte arguments; empty captures |
| streams | Write exactly 64 KiB of x on stdout and y on stderr |
| input | Read/validate/echo 128 KiB of x |
| environment | Verify exactly one variable PROBE=goose |
| redirect | Same writes as streams; capture stdout, discard stderr |

Rust uses Command and wait_with_output, Go uses Cmd.Run with byte buffers, and
Python uses Popen.communicate. Rust starts and joins one scoped writer thread per input call to avoid
pipe deadlock; Go uses its reader copier and Python its communicate loop. Goose
feeds a finite copied byte slice through a socket; the baselines use pipes.
Goose retains its 10-second deadline and hard per-stream cap (64 KiB, or 128 KiB
for input); baseline captures are **uncapped with no timeout**. Goose additionally
resets all child signal dispositions/mask and closes all child FDs >=3; baseline
policies differ. The outer scheduler has a 120-second process timeout to detect
a broken driver, not to supply equivalent per-child cancellation. These runs
cannot measure equal security guarantees, overflow behavior or failure cleanup.

Goose reuses caller capture capacity across calls; Rust, Go and Python use newly
returned/owned captures as their standard capture APIs ordinarily do. Goose
still clears/zero-fills its configured buffers and stages argv/input each call.
The results describe these complete public API paths, not language speed or
native spawn cost in isolation. No policy is weakened for a timing advantage.

The scheduler shuffles all 24 language/fixture blocks in every round with seed
3025. CPU affinity is inherited by drivers and children. This is a shared host,
not exclusive CPU isolation; affinity does not control SMT siblings, other
processes, filesystem cache, temperature or frequency. Raw results retain all
sample outputs, order, medians, toolchain versions, flags, executable/source
SHA-256 hashes, source commit/dirty state and platform. Sources are fingerprinted
before building and verified after building and measurement; binaries are hashed
before and after sampling. A changed artifact aborts the run. A failed fixture aborts
the run; it is not silently omitted. Inspect samples/ranges before interpreting
small median differences. Reproduce on your target system before choosing an
implementation for performance.

See [the feature and semantic comparison](../../docs/COMPARISON.md) and
[the execution contract](../../docs/API.md). A matched safety-contract comparison
would need independently validated bounded-output/deadline/FD adapters for all
baselines, with their work included in timing. This harness makes no such claim.
