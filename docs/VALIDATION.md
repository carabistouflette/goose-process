# Integration-readiness validation

Validated 2026-11-04 on Linux x86-64 with the compiler in GOOSE_REVISION.
Production source is captured by
[`191a9423197e850e699eff500620a5c7d111aa54`](https://github.com/carabistouflette/goose-process/commit/191a9423197e850e699eff500620a5c7d111aa54).
Documentation-only commits after this snapshot do not change the native engine.

The public interpreter/native/debug suites, 244 independent subprocess oracle
cases and 2,536 refused-execution validation classifications passed. Both GCC
and Clang fortified ASan/UBSan suites passed, covering stream/FD/environment,
resource cleanup, syscall failures, lifecycle, cancellation and deterministic
clock behavior. Validation fuzzing completed 10,000 runs with execution refused.
[Hosted GCC/Clang CI](https://github.com/carabistouflette/goose-process/actions/runs/41835093661)
passed on the published main commit. A separate HTTPS checkout with its own
pinned compiler passed make check test; the public README import was exercised.

Integration-specific regressions verify:

- Reaped child identity is hidden even while captures remain undrained.
- Signaling after reap is refused without a signaling syscall.
- Exceptional child responsibility transfers exactly once across repeated calls.
- Only explicit start/Child accepts timeout_ms=0; synchronous run stays finite.
- A no-deadline Child remains active after a 48-hour synthetic clock advance,
  while a 10 ms step interrupted twice retains its 10 ms/4 ms remaining budgets.
- An unbounded wait uses poll(-1); step(0) still polls without blocking.
- Capture overflow, signaling, reaping and cleanup still apply without an
  overall deadline. Negative/excessive budgets and finite defaults remain checked.

Separate lifecycle, security/benchmark and final publication reviews found no
remaining library blocker in the examined changes. Source review and targeted
probes are not exhaustive race analysis or a proof of correctness.

The subsequent [reference caller regression](../tests/caller.c) adds three scoped
application scenarios: direct startup cancellation, lock retention during graceful
cleanup, and exceptional transfer with denied recovery waits. Both independent
reviewers approved source SHA-256
`670978b8adb1b36ba93afd22a0f9cd241b6344dfaeedcb93f0413212f1c36658`.
Normal execution and GCC/Clang fortified ASan/UBSan passed. It is a private-ABI
test, not a public C adapter or proof of Honk behavior; terminal launch timing
and arbitrary signal storms remain outside these scenarios.

[Standard-library results](../bench/compare/RESULTS.md) retain their successful-
workload policy differences. [Shared-engine profiling](../bench/compare/SHARED.md)
uses the same C kernel for both front ends, so shared defects remain shared;
it does not independently prove the engine safe or rank Rust std::process.
No reliable wrapper bottleneck was established in that session.

The 0.1.0 release tag remains gated on Honk's real-caller signal forwarding,
terminal duplicate suppression, lock retention and transferred-child recovery
tests. Library-level validation does not establish those application properties.
