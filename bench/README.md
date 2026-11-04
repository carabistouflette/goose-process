# Process benchmark

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
