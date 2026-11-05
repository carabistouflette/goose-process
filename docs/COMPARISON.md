# Subprocess comparison and acceptance scope

## Implemented Linux target

Current main supports the workflows below on Linux/glibc. See the [API](API.md)
for defaults, limits and cleanup rules. Streaming and pipelines are not in v0.1.0.

| Workflow | Current Goose interface | Regression coverage |
| --- | --- | --- |
| Explicit argv/cwd and opt-in executable lookup | run/start; effective child PATH; no shell fallback | API, independent argv oracle, extended native tests |
| Per-child environment | inherit, clear, override, remove, empty value | extended native tests; validation fuzzing |
| Independent binary stdio | capture, inherit, null, borrowed FD; stderr merge | extended native and closed-stdio fault tests |
| Finite stdin | up to 1 MiB; concurrent feed/drain; one-way socket transport | binary roundtrip, early closure, SIGPIPE/fault tests |
| Incremental stdin | start-only Input.Stream; io_step acceptance counts and explicit close_stdin; no payload queue | >1 MiB binary input, duplex backpressure, worker frames, early closure and error counts |
| Child control | start, step, try_wait, wait, signal, terminate, kill, close | lifecycle tests; compile-time owner/view checks |
| Bounded capture and deadlines | per-stream caps; direct-child cancellation/reaping | limits, deterministic deadlines, cancellation failures |
| Bounded live output | explicit Stream routes and caller buffers via stream_step; mixed capture | binary order, backpressure, merged output, input, reaping-before-drain and cleanup faults |
| Two-stage pipelines | noncopyable Pipeline, shared positive deadline, finite input, three captures and per-stage outcomes | pipe saturation, early consumer closure, partial launches, dual cleanup transfers and descendant-held capture |

These interfaces cover the listed Linux workflows. They do not implement every
method or platform in the three standard libraries. Differences include fixed limits, finite deadlines by default,
socket rather than FIFO finite stdin, explicit Child/Pipeline cleanup, exactly two
pipeline stages with restricted routing, and no process-group policy, credentials, arbitrary extra-FD allowlist, PTYs,
Windows or macOS support. Fd routing permits caller-owned live stdio streams.

The [comparison harness](../bench/compare/README.md) measures the native API track
with identical successful fixture bytes/status. It does not claim equal safety
contracts: baseline capture APIs omit Goose's enforced caps/deadline and some
descriptor/signal policies. Measured results must retain these qualifications.

## Older comparisons

[Historical feature research](COMPARISON-HISTORY.md) describes an earlier API.
[Benchmark source scope](../bench/CURRENTNESS.md) identifies which measurements
match current code. Historical cross-language tables do not rank current main.
