# Historical process benchmark

This measured an older implementation. It is not a current-main performance
result. See [source currentness](CURRENTNESS.md).

# Process benchmark

The [two-stage pipeline route study](PIPELINES.md) retains a separate checked
comparison of convenience copies and owned borrowed captures, including methods,
raw samples and limits. It measures complete API routes through the same engine.

For the resident streaming resource/latency soak and separate instrumented
profiles, see [the sustained streaming evidence](STREAM-SOAK.md). It retains the
full measured run and failed profiler attempts without a general speed claim.

For the checked Rust/Go/Python successful-workload comparison, see
[the comparative harness and its contract differences](compare/README.md).
Its [recorded results](compare/RESULTS.md) include checked Rust/Go/Python/Goose
workloads and raw samples; standard-library safety contracts differ.

`make bench BENCH_ITERATIONS=100` builds an optimized native Goose harness with
GCC by default (`BENCH_CC=clang` selects Clang), warms reusable buffers once per
mode, and records seven samples in ignored `.build/bench.json`.

The fixtures spawn `/bin/true` with default captures, inherited streams, maximum
capture buffers and a large argument array, plus the test helper emitting 64 KiB
or 1 MiB on each stream. Each result is checked. Measurements include
argument validation/copying, buffer initialization, pipes, spawning, completion
and cleanup, plus loop/status-check overhead. They exclude compilation and
warmup. Mode order is fixed; CPU load, filesystem cache and scheduling affect
results. They do not measure long-running programs or general streaming throughput.

The JSON retains raw samples/stdout, iteration count, compiler/version/flags,
Goose version, CPU/platform, source revision/dirty state and native fingerprint.
Numbers are local end-to-end costs, not a cross-library ranking or performance
guarantee. CI uses small smoke workloads and has no timing threshold. Preserve
validation, descriptor closure and cancellation semantics when optimizing.

For comparisons, build baseline/current executables with the same compiler,
flags and harness, with `helper` beside each. Pass `--executable PATH
--baseline PATH --baseline-revision SHA` to `bench/measure.py`, alongside
`--goose PATH --cc gcc --iterations 100 --runs 11`. Invocation order alternates
each round; fixture order within each invocation stays fixed. The report stores
both variants' samples, execution order and executable hashes. Retain exact
source/harness snapshots when measuring dirty trees. Interleaving reduces batch
effects but does not eliminate scheduler/frequency noise or establish universal
performance improvements.

`make profile` additionally requires `strace` and records parent-side `wait4`,
`read` and `poll` counts in `.build/syscalls.txt` for three native-kernel calls
capturing 1 MiB per stream. This fixture excludes Goose buffer initialization
and does not time the public API. Pidfd readiness gates child waits, avoiding
one wait syscall per capture iteration. Keep tracing results separate from
untraced timing samples: tracing changes scheduling and execution cost.

## Recorded local results

Recorded on 2026-11-04 from clean source
[`9fd737c08f491ff842725c16189073891c8cfff7`](https://github.com/carabistouflette/goose-process/commit/9fd737c08f491ff842725c16189073891c8cfff7).
Hardware: Intel(R) Core(TM) Ultra 7 258V. Platform: Linux-7.2.11-arch1-1-x86_64-with-glibc2.44.
Compiler: gcc (GCC) 16.2.1 20260810; flags `-O2`. Goose: `goose eadca40 (runtime 3025e5810379)`.
Seven samples, 100 calls per sample and one warmup per fixture.

These measurements came from a shared workstation with concurrent build
activity, without CPU pinning or frequency control. Scheduling noise is visible
in the ranges. They establish a reproducible local workload, not a speed ranking
or an optimization speedup. Units are **microseconds per call**.

| Fixture | Median | Sample range |
| --- | ---: | ---: |
| `capture` | 1634.0 | 1240.7–2397.7 |
| `inherit` | 1276.0 | 992.2–1645.3 |
| `maxcap-empty` | 1871.8 | 1488.4–3208.5 |
| `streams-64k` | 1584.8 | 1110.1–2941.4 |
| `streams-1m` | 2984.7 | 2557.5–4408.0 |
| `large-argv` | 2002.9 | 812.7–2720.0 |

[Raw samples and complete provenance](results/linux-gcc-2026-11-04.json) include
all fixture results, compiler/runtime versions, source fingerprint, clean-tree
state, executable hash and execution order. Reproduce with `make setup` followed
by `make bench BENCH_ITERATIONS=100 BENCH_CC=gcc`; measurements will vary by host.
