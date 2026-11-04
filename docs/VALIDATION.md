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

## Real caller acceptance for 0.1.0

Honk's published process-consumer implementation is
[`db81d8fe552b233231ff8ff29af9e75761484cd6`](https://github.com/carabistouflette/honk/commit/db81d8fe552b233231ff8ff29af9e75761484cd6),
using unmodified goose-process `191a9423197e850e699eff500620a5c7d111aa54`.
Its [validation/profile checkpoint](https://github.com/carabistouflette/honk/blob/a36ca656da0467783c89a6f491e0b1206a60c05d/docs/PROCESS-CONSUMER-2026-11-04.md)
and [exact-checkpoint GCC/Clang CI](https://github.com/carabistouflette/honk/actions/runs/41846517978)
are public. This is a feature-branch acceptance checkpoint; promotion to Honk
main is held for a separate integration and is not represented as complete here.

The owner ran normal/security, GCC/Clang sanitizer, portable and static checks.
Existing CLI cases cover direct and registered terminal signals and lock cleanup;
four new process methods cover exact stderr, inherited stdin, capture/argument
bounds, pre-start no-launch cancellation, post-start pidfd forwarding and actual
exceptional transfer/recovery under denied waits. The native fault compiler has
distinct content-addressed paths for source, compiler and instrumentation, avoiding
normal-versus-sanitized native cache reuse. The unconditional stale guard also
rejects under Python optimization. Launch-window terminal timing remains best effort.

Independent verification used a fresh public HTTPS clone at exact `db81d8f`,
the pinned Goose compiler and an isolated Honk cache. Vendor checks/build, all
four focused methods, frozen self-check and frozen packaging passed. The tracked
tree remained clean; the exported archive contained the exact seven library
source/build/header/license files plus unchanged manifest/lock, with no generated
state. Separate Standards/Spec reviews found no production mismatch; the small
test-cache validation findings were fixed before publication.

All 54 paired caller samples, summaries and 34 recorded source identities were
independently verified against the immutable profile checkpoint. These measure
whole implementations on a shared host; baseline/source and RSS limitations
remain explicit in Honk's report. They do not establish universal speed or memory
superiority. Library tests alone do not establish arbitrary application correctness.

## Post-release caller main validation

Honk subsequently promoted the reviewed integrations to public main at
`453aa53dc7b20622782423c64cc180097f653650`, with green GCC/Clang CI.
The [controlled real-service soak](../bench/SOAK.md) passed 24 checked runs from
an isolated public checkout. No library runtime fix was justified; this bounded
workload does not establish general service reliability or memory behavior.
