# Process benchmark

`make bench BENCH_ITERATIONS=100` builds an optimized native Goose harness with
GCC by default (`BENCH_CC=clang` selects Clang), warms reusable buffers once per
mode, and records seven samples in ignored `.build/bench.json`.

The two modes repeatedly spawn `/bin/true`, first capturing both streams and
then inheriting stdout/stderr. Each result is checked. Measurements include
argument validation/copying, buffer initialization, pipes, spawning, completion
and cleanup, plus loop/status-check overhead. They exclude compilation and
warmup. Mode order is fixed; CPU load, filesystem cache and scheduling affect
results. They do not measure long-running programs or streaming throughput.

The JSON retains raw samples/stdout, iteration count, compiler/version/flags,
Goose version, CPU/platform, source revision/dirty state and native fingerprint.
Numbers are local end-to-end costs, not a cross-library ranking or performance
guarantee. CI uses small smoke workloads and has no timing threshold. Preserve
validation, descriptor closure and cancellation semantics when optimizing.
