# Managed two-stage pipelines

This is the implementation contract for GL-PIPELINE-01. The interface remains
experimental. It connects exactly two literal argv commands through one owned,
blocking OS pipe, without a shell or process-tree supervision.

## Public interface

`Pipeline` is a noncopyable resource, owning finite input, final stdout, both
stage stderr captures and native supervision state. Its operations are:

- `start_pipeline(first_args, first_options, second_args, second_options, owner)`.
- `pipeline_step(owner, timeout_ms=0)` returns PipelineStatus and done. One call
  performs bounded fair I/O work; a zero slice still checks ready descriptors.
- `wait_pipeline(owner)` pumps until completion under the shared deadline.
- `close_pipeline(owner)` force-cancels both owned children and completes or
  explicitly transfers cleanup ownership. It also closes a partially started
  pipeline. It is not graceful signal forwarding.
- `pipeline_stdout`, `pipeline_first_stderr`, `pipeline_second_stderr` borrow
  retained binary prefixes. Mutating operations invalidate earlier views.
- `run_pipeline(first_args, first_options, second_args, second_options, out,
  first_err, second_err)` is the synchronous convenience operation.

`PipelineStatus` contains overall error/os_error, first and second Status values,
and first_launched/second_launched booleans. Launch flags record actual successful
posix_spawn, including a later pidfd/setup failure. Each stage independently
reports its exit/signal, operation error and one-time unreaped_pid transfer.
Unlaunched stages have exit_code=-1. Pipeline success requires overall Error.None
and successful completion of both stages. Ordinary nonzero exits and signals,
including upstream SIGPIPE after early consumer closure, remain command outcomes;
they do not trigger fail-fast cancellation of the other stage.

Both Options.timeout_ms values must be equal and positive, at most MAX_TIMEOUT_MS.
They describe one overall monotonic budget, not sequential per-stage budgets.
Stage cwd/environment/explicit PATH policy and existing argv/text limits apply.
Initial routing is intentionally restricted and validated before any launch:

- Both capture flags are true. Both stderr modes and second stdout are Default
  or Capture, with independent existing limit overrides up to 1 MiB each.
- First stdout is Default with default stdout_limit/stdout_fd. In this API that
  reserves the pipeline connection; first output_limit controls its stderr.
- First stdin is Null or finite Bytes, with at most 1 MiB copied input.
- Second stdin is Null with empty input and default stdin_fd. In this API that
  reserves the connection from the first stage.
- Explicit conflicting Stream/Fd/Inherit/Null/merge output routes and all
  conflicting descriptor/input settings are rejected before either launch.

Managed captures hold at most 3 MiB payload, plus finite input; interstage bytes
are not accumulated in userspace and may exceed the finite input/capture limits.
The kernel pipe supplies bounded backpressure. No launch is transactional:
downstream may execute side effects before an upstream launch subsequently fails.

## Ownership, scheduling and cancellation

Prepare/validate both commands before launch. Start one checked absolute deadline
before native command preparation/FD setup and use that exact deadline in both
spawn paths and the combined pump. Existing single-child deadline semantics stay
unchanged. Neither spawn nor OS scheduling provides a strict completion bound.

Create one blocking CLOEXEC pipe with descriptors promoted above standard FDs.
Launch the consumer first. Close the parent's read end immediately after consumer
setup and its write end immediately after producer setup, on every path. Native
snapshotting and close-from routing prevent accidental endpoint inheritance.

Poll producer input, three captures and both pidfds together. Each ready stream
gets at most the existing bounded 16-KiB read/write quantum per round. Reap ready
children without treating an exit as capture EOF. Disable closed/completed FDs
to avoid persistent-readiness spin. Never serially wait one stage while the other
needs feeding or draining. Continue draining after an ordinary stage exit.

On close, deadline, overflow or system failure, close parent endpoints and
request cancellation for both live children before either blocking wait. Then
attempt cleanup of each independently. A cleanup denial dominates the overall
operation error as SystemError, while preserving both stage diagnostics and
transfers. Reaped children are never signaled. No global signal handler is added;
normal callers can latch signals and close from their guarded step loop.

Each exceptional PID is emitted once. The Pipeline is closed after transfer;
repeated close does not recover that PID or emit it again. The caller must retain
its execution scope/locks and recover every transferred child according to
CALLERS.md. Descendants retaining a capture may force timeout; descendants are
not claimed to be supervised or killed by this interface.

Native state is private: 55 i64 words (seven pipeline fields plus two unchanged
24-word child states). Native metadata is 17 words: overall errno at0; first and
second result codes at1/2; child metadata at3..8 and9..14; launch flags at15/16.
The exported native signatures and full span/alias validation must agree with
the public wrapper. No caller input/output pointer is retained natively.

## Acceptance

Cover binary input and arguments, no-shell semantics, exact/zero/overflow caps
for every capture, simultaneous stream pressure above pipe capacity, early
consumer closure/SIGPIPE, independent stage statuses and owner reuse. Validate
all conflicting modes/second-stage malformed input before any launch.

Fault fixtures must cover both launch failures, successful spawn followed by
pidfd/setup failure, the shared deadline across delayed launch/EINTR, cancellation
of both children before wait, independent cleanup denials/transfers, descriptor
isolation/leaks, closed standard descriptors, and descendant-held captures.
Compile-time checks reject copied owners and capture views surviving mutation.

A real example sends C source through `/usr/bin/printf` into
`/usr/bin/cc -x c -fsyntax-only -`, checking positive and negative compilation.
Run normal GCC/Clang, both sanitizers, intercepted-spawn fuzzing and static
analysis; independently review implementation and evidence. Retain measured
benchmark methods/samples/limits without claiming universal speed or safety.
Publish only after exact-head hosted CI and standalone-clone checks pass.
