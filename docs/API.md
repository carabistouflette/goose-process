# API and execution contract

## Public API

`process::run(args, options, out, err) -> Status` runs one command synchronously.
`args` is a read-only slice of read-only byte slices, including the executable as
`args[0]`. It must be an absolute path. There is no PATH search, shell parsing or
fallback execution of malformed executables. Arguments preserve every byte
except NUL, which is rejected. Empty arguments after `args[0]` are valid.
Executable and argument storage is copied before spawning and never retained.

| Option | Default | Meaning |
| --- | --- | --- |
| `cwd` | `""` | Inherit working directory; nonempty paths select child cwd |
| `capture` | `true` | Capture stdout and stderr separately; false inherits both |
| `timeout_ms` | `30000` | Positive cancellation budget, measured monotonically |
| `output_limit` | `65536` | Maximum bytes retained per captured stream |

The executable remains absolute even when cwd is supplied. Relative cwd is
resolved from the parent's current directory. Stdin is always `/dev/null`.
The child inherits the environment. All descriptors numbered 3 and above are
closed in the child, including caller descriptors lacking `FD_CLOEXEC`. The
parent's descriptors and signal dispositions are unchanged. Child signal
dispositions are reset to defaults and its signal mask is cleared.

Both buffers are cleared even on invalid input or launch failure. Captured
output is arbitrary bytes, with no decoding or newline handling. Successful
captures are complete. Timeout/output-limit/system failures may retain only a
prefix of each stream. Streams have no combined ordering; cancellation can
occur before either stream is fully drained. Exactly `output_limit` bytes is
accepted; an additional observed byte triggers `OutputLimit`. A zero limit
accepts empty output and rejects the first nonempty read. Output limits are
validated even with capture disabled; disabled capture returns empty buffers.

Inputs cannot borrow either output buffer; Goose rejects such calls. Use distinct
output buffers. Overlapping nonempty output spans are rejected before spawning;
sharing an empty buffer with capture disabled or a zero limit has no observable
capture conflict. Capacity is reusable but successful calls do not retain prior
content. Implementation fields/helpers and the native ABI are not public API.
Native callers must supply valid pointer/length pairs; validation does not make
arbitrary C pointers safe.

## Status

| Field | Meaning |
| --- | --- |
| `error` | `None`, `InvalidInput`, `LaunchFailed`, `SystemError`, `TimedOut`, or `OutputLimit` |
| `os_error` | Linux errno for launch/system failures; otherwise zero |
| `exit_code` | Actual reaped normal exit status (0–255), or -1 |
| `signal` | Actual reaped termination signal, or 0 |
| `unreaped_pid` | Normally 0; exceptionally a live/unreaped child transferred to caller |

`process::success(status)` means `error == None && exit_code == 0`.
`None` can accompany a nonzero exit code or signal termination. A timeout or
output-limit result may include the child's actual status if it has been reaped;
`SIGKILL` commonly reflects cancellation. Exit/signal fields never convert an
operation error into success. `LaunchFailed` includes an invalid/unavailable
cwd as well as executable failures. `InvalidInput` spawns nothing. Diagnostics
are enum/errno values and never include argument or environment content.

The cancellation deadline starts during native preparation, before spawning.
Both streams are drained concurrently with bounded work between deadline
checks. A pidfd makes child completion pollable without periodic sleeps. On
timeout, overflow or post-launch system error, the runner requests `SIGKILL`
and reaps its direct child. If pidfd signaling is unavailable/denied, it falls
back to signaling that unreaped PID. Already-reaped PIDs are never signaled.
Stopped or traced-child notifications are not termination: the deadline still
applies, and cleanup waits for a terminal exit/signal status before releasing
child ownership.

If **both cancellation mechanisms fail**, cleanup performs a nonblocking wait
and returns `SystemError` with the cancellation errno. A nonzero `unreaped_pid`
explicitly transfers ownership: the application must arrange termination and
reaping of that child. This exceptional field does not identify a completed
process to signal later. It is zero when the child has already been reaped.
If reaping itself fails with an error other than `ECHILD`, the runner likewise
returns `SystemError`, the wait errno and `unreaped_pid`; responsibility for
eventual reaping transfers to the caller even if cancellation was delivered.

Timeout is a cancellation deadline, not a strict wall-clock bound: process
creation, kernel/filesystem work and reaping after a delivered signal can take
longer. Descendants are not supervised or killed. A descendant holding capture
pipes open can produce `TimedOut` even after the direct child exits normally;
the remaining pipes are closed at cancellation. Descendants may then receive
SIGPIPE when writing to them.

## Host responsibilities

Calls use no global mutable library state and can run concurrently with separate
buffers. The host must not externally reap these children, alter `SIGCHLD`
disposition during calls, or concurrently mutate the process environment or cwd.
`SIGCHLD=SIG_IGN` and `SA_NOCLDWAIT` are rejected before spawning. Custom SIGCHLD
handlers may notify the host but must not reap runner-owned children. Thread
cancellation during `run` is unsupported. Goose-managed buffer allocation
failures follow its runtime policy and are not recoverable status values.
Failure to allocate the native argument copy returns `SystemError`/`ENOMEM`.

This library does not remove environment credentials, enforce filesystem/network
isolation, or sanitize a command's own interpretation of options and scripts.
Only run executables appropriate for the application. Cancellation/reaping
depends on host OS permissions. Linux kernel 5.3+ (pidfds) and glibc 2.34+
(`posix_spawn` close-from actions) are required; no silent fallback weakens
descriptor isolation on unsupported platforms.

## Bounds and costs

| Public constant | Limit |
| --- | --- |
| `MAX_ARGUMENTS` | 256 including executable |
| `MAX_ARGUMENT_BYTES` | 65,536 including one NUL terminator per argument |
| `MAX_ARGUMENT_LENGTH` | 16,384 bytes per argument |
| `MAX_PATH_BYTES` | 4,095 bytes each for executable and cwd |
| `MAX_OUTPUT_BYTES` | 1,048,576 bytes per stream |
| `MAX_TIMEOUT_MS` | 86,400,000 (24 hours); minimum 1 |

Limits are immutable and checked in Goose and at the native boundary. Native
metadata must not overlap inputs or outputs, and output spans must not overlap.
Logical retained captures total at most 2 MiB. Preparation scans/copies argument
bytes and initializes up to two output-limit-sized buffers. The C kernel has a
bounded argument-pointer array, path buffer, and 16 KiB read scratch; argument
text is allocated once and freed. Underlying allocators may retain high-water
capacity. These are logical payload/work bounds, not exact allocator budgets.
Every call still pays OS process-creation costs. The deadline and capture limit
bound draining even when descendants keep a pipe open or output never stops.
