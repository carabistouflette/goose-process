# Integration-readiness validation

## Managed pipelines, 2026-11-05

The [two-stage contract](PIPELINES.md) and real
[compiler example](../examples/pipeline.goose) add an owned blocking connection,
one positive shared deadline and separate stage outcomes. Runtime source is
`0abb97216080740e5b0d50fb3138297b9505914c`; the clean measured checkpoint is
`396ada2b8d9a55a22d405a77833f4a03e6aa889d`.

Both GCC and Clang full `make check test` suites passed, covering interpreter,
optimized and debug/no-BCE public pipeline execution and all legacy cases.
Both fortified ASan/UBSan suites passed, as did 10,000 Clang/libFuzzer runs with
all mutated execution refused and separate Clang static analysis of both native
translation units. Seven compile-time ownership/view rejection checks passed.

New public cases verify binary argv/input, simultaneous pipe/capture pressure,
every capture's exact/zero/overflow boundaries, ordinary independent exits and
upstream SIGPIPE, shared timeout, step/cancel, repeated completion/close, owner
reuse, staging aliases and distinct cwd/environment/effective PATH per stage.
Conflicting routing and malformed commands are rejected before either launch.

The native fixture verifies downstream/upstream launch failure, successful spawn
followed by pidfd failure, a synthetic deadline spanning both launches, EINTR,
both cancellation requests before either blocking wait, independent and dual
one-time PID transfers under scoped wait/signal denials, descriptor isolation,
all seven closed-standard-descriptor combinations and descriptor/child cleanup.
A descendant retaining captures proves timeout after both direct children exit;
the test explicitly adopts, signals by owned pidfd and reaps that descendant.
This demonstrates the supervision boundary rather than process-tree cleanup.

The optional published checker also passed the actual positive/negative C
compiler pipeline example and all previous consumer controls. Its dependency pin
and source were not changed. Independent native/API and final Standards/Spec
reviews reported no blocking finding; these were static reviews, not exhaustive
concurrency proofs. The [pipeline route report](../bench/PIPELINES.md) retains all
180 measured calls and 36 warmups with unchanged source/tool/binary identities,
shared-host and short-run limitations, and no universal speed or memory claim.

A credential-disabled anonymous HTTPS checkout of
`141c8b38a7087592e3d356d14a4bd3a011b2495b` passed the complete GCC check/test
suite and all four unmeasured benchmark protocol cells. Both checkout and fresh
`GOOSE_HOME` paths contained spaces, using the existing exact pinned compiler;
this is not a cold compiler-bootstrap claim. The tested tree stayed clean.
The first standalone attempt exposed unquoted absolute helper arguments in Make;
the quoted-path fix changed build/test invocations only and the fresh rerun passed.
An independent evidence review matched every raw measurement and its source
identities to the immutable measured commit, verified summary arithmetic and
reported no publication blocker in the reviewed sources/docs.

The published implementation is `7f759bb22e4afcbe65c78dee20861509c3310a52`.
Its [main GCC/Clang workflow](https://github.com/carabistouflette/goose-process/actions/runs/42091612920)
completed successfully, as did the separate feature and PR workflows. This is
validation of that source, not proof of correctness for every application.

[Earlier Child/caller records](VALIDATION-HISTORY.md) preserve their source
versions. [Benchmark currentness](../bench/CURRENTNESS.md) separates old results
from measurements that match this implementation.
