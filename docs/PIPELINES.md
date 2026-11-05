# Managed two-stage pipelines

Connect two commands through a blocking pipe, without a shell. Both share a
deadline and each reports its own exit or signal. This API is on main, not v0.1.0.

After `make setup`, run the compiler example:

```sh
.toolchain/goose/build/goose run examples/pipeline.goose
```

It sends valid and invalid C source through `/usr/bin/printf` into `/usr/bin/cc`
and prints `pipeline compiler checks passed`. Both host tools must be installed.

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
The synchronous convenience function copies the three retained prefixes into
caller buffers, temporarily adding up to 3 MiB output payload. Use the owner
accessors to inspect captures without that convenience copy.

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

See [validation records](VALIDATION.md) for tests and implementation checks.
