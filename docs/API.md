# API and execution contract

These docs describe main. Streaming and managed pipelines are not in `v0.1.0`;
see [version scope](../README.md#main-versus-release).

## Commands and configuration

`process::run(args, options, out, err) -> Status` executes one command and waits
for its direct child and captured stream EOF. `args` includes the executable as
its first byte slice. The default requires an absolute executable path. Arguments
preserve all bytes except NUL; empty arguments after the executable are valid.
There is no shell parsing or shell fallback for malformed executables.

| Option | Default | Meaning |
| --- | --- | --- |
| `cwd` | `""` | Inherit cwd; otherwise select child cwd |
| `capture` | `true` | Resolve `Output.Default` to Capture, or Inherit when false |
| `timeout_ms` | `30000` | Monotonic cancellation budget; only start/Child accepts explicit 0 for no overall deadline |
| `output_limit` | `65536` | Default maximum retained bytes per captured stream |
| `stdout`, `stderr` | `Output.Default` | Independently select output routing |
| `stdin` | `Input.Null` | Select stdin routing |
| `input` | `""` | Finite binary input, valid only with `Input.Bytes` |
| `stdout_fd`, `stderr_fd`, `stdin_fd` | `-1` | Borrowed source FD for the corresponding Fd mode |
| `stdout_limit`, `stderr_limit` | `-1` | Override output_limit; -1 uses the default |
| `clear_environment` | `false` | Start with an empty environment instead of inheriting |
| `environment` | empty | Slice of `EnvOverride { key, value, remove }` |
| `search_path` | `false` | Explicitly enable PATH search and relative executables |

`Output` supports Default, Capture, Inherit, Null, Fd, Stream, and **stderr-only** Stdout.
Stdout merges stderr into the child's already-routed stdout. A merged capture
uses only the stdout limit and returns an empty stderr buffer. Separate captures
have no combined ordering. Null discards output through `/dev/null`.

`Input` supports Null (`/dev/null`, read-only), Inherit, Bytes, Fd, and Stream. Bytes
copies at most 1 MiB before execution and sends it while draining both captures.
It uses a **one-way Unix socket byte stream**, not a FIFO, to suppress write-side
SIGPIPE with `MSG_NOSIGNAL` without changing the caller's signal state. Empty
input delivers EOF. Early child-side closure is normal; unsent input is discarded.
For programs requiring a real pipe, terminal, file or other descriptor type, use
Fd. The library does not feed or manage a caller-supplied stdin FD.
Stream is start-only: offer bounded views with `io_step` and explicitly close
stdin when finished. See [incremental input](#bounded-incremental-stdin).

Fd modes snapshot valid caller descriptor numbers during start. Sources must
remain open and stable until `start`/`run` returns from launching; the caller may
then close its copy. Child routing supports FD cycles (for example stdout/stderr
swaps) without changing parent FD flags. FDs inherit their source's file offset,
access mode and blocking behavior. All child descriptors numbered 3 and above
are closed after routing. Inherit preserves the corresponding standard FD,
including a closed standard FD. Parent descriptors and signal dispositions are
unchanged. The child's signal dispositions reset to defaults and mask is cleared.

`EnvOverride` sets `key=value` by default. With `remove: true`, value must be empty
and all inherited entries matching that key are removed. Empty values are valid.
Keys must be nonempty and contain neither NUL nor `=`. Duplicate explicit keys,
including a set/remove conflict, are rejected. Inherited duplicates matching an
override are all replaced; other inherited entries retain their order. Environment
changes apply only to the child; the library does not redact credentials by default.

PATH lookup uses the **effective child environment**. An absent PATH defaults to
`/bin:/usr/bin`; an empty PATH or empty component searches child cwd. Relative
PATH components resolve after the child cwd change. A name containing `/` is
used directly and does not inspect PATH; relative names require `search_path`.
Search continues after ENOENT/ENOTDIR/EACCES, returns EACCES if any candidate was
denied, and stops on other errors including ENOEXEC. No implicit shell is invoked.
Enabling search trusts the selected PATH and cwd; use absolute paths for stronger
executable selection.

## Outputs and status

`run` replaces both output buffers even on invalid input. All arguments, cwd,
environment changes and finite input are staged before replacement: an input view
may borrow a previous output. Use distinct output owners. Overlapping nonempty
native output spans are rejected before spawning. Captures are arbitrary bytes;
no decoding or newline transformation occurs. Successful captures are complete.
Failures may retain only prefixes. Exactly the configured limit is accepted;
one additional observed byte triggers OutputLimit. A zero limit accepts empty
output and rejects the first nonempty read. All limits are validated even for
uncaptured streams. Allocator capacity may be reused between calls.

| Status field | Meaning |
| --- | --- |
| `error` | None, InvalidInput, LaunchFailed, SystemError, TimedOut, OutputLimit, InvalidState |
| `os_error` | Linux errno for launch/system failures; otherwise zero |
| `exit_code` | Actual reaped normal exit status 0–255, or -1 |
| `signal` | Actual reaped termination signal, or 0 |
| `unreaped_pid` | Normally 0; exceptional transfer of termination/reaping responsibility |

`success(status)` means `error == None && exit_code == 0`. None may accompany
nonzero exit or signal termination. LaunchFailed includes unavailable cwd and
executable failures, and is distinct from a child exiting 126/127. InvalidInput
spawns nothing. Diagnostics never contain argument/environment contents.

## Child lifecycle and cancellation

See [application signal guards and exceptional ownership](CALLERS.md) for a
caller integration algorithm and links to the maintained lifecycle tests.

Declare `var child = process::Child { .. };` as a local or global owner.
`Child` is a compiler-enforced noncopyable root; unannotated aliases borrow it.
It cannot be stored by value in arrays/other structs or sent to another thread.
Its underscore fields and the native ABI are implementation details, not public
API. Do not modify them directly. There is **no automatic destructor or background
watchdog**: always finish an active child with wait, repeated step, repeated
stream_step for streamed output, io_step for streamed input, or close.

| Function | Contract |
| --- | --- |
| `start(args, options, child)` | Launch; copy finite input and allocate captures; reject an active owner |
| `step(child, timeout_ms = 0)` | Feed/drain at most one bounded IO iteration; return Status and completion flag |
| `wait(child)` | Feed/drain to EOF/completion or cancellation |
| `io_step(child, input, out, err, timeout_ms = 0)` | Input.Stream only; bounded duplex progress with named per-call counts |
| `close_stdin(child)` | Input.Stream only; close local write endpoint without waiting or draining |
| `try_wait(child)` | Nonblocking status observation/reaping; return Status and whether direct child reaped |
| `send_signal(child, signal)` | Signal unreaped direct child by pidfd; zero tests signal permission/existence |
| `terminate(child)`, `kill(child)` | Send SIGTERM or SIGKILL; still call wait/step/close |
| `close(child)` | Discard remaining IO, cancel/reap active child; idempotent |
| `id(child)` | Unreaped direct-child PID, or -1 after reaping/completion; use send_signal for signaling |
| `stdout(child)`, `stderr(child)` | Read-only views of collected prefixes; consume/copy before a mutating operation |

A step timeout ranges from 0 to 24 hours; zero polls once without waiting.
Its slice uses an absolute monotonic deadline even under EINTR. The overall
process deadline is checked during IO operations, not while an idle Child is
held. try_wait does **not** feed input or drain output; a child blocked on IO may
never finish through try_wait alone. Even when try_wait reaps the direct child,
call the appropriate wait/step/stream_step/io_step operation to collect remaining
output, or close to discard it. A completed
wait may be repeated. An unsuccessful start may replace previous captures;
rejection of an already-active owner leaves it unchanged. An owner can be reused
after completion/close. Views cannot outlive
operations that resize captures; the Goose checker enforces this lifetime rule.

After reaping, id returns -1 even when descendant-held captures still need
draining. With no external reaper and no intervening operation on the owner,
a positive id remains reserved until the library reaps it: natural exit leaves
a zombie. An immediate observational query such as getpgid may still fail with
ESRCH. Do not store the numeric PID across wait/step/try_wait or use it to signal;
send_signal checks owner state and uses a pidfd instead.

Synchronous run always requires a finite positive deadline. For a supervised
Child, explicitly selecting timeout_ms=0 disables only the overall deadline;
capture/input bounds, step slice deadlines, signaling and cleanup are unchanged.
The application must still pump or finish that owner. wait on such an owner can
block indefinitely, including when descendants keep capture pipes open. Defaults
remain finite; there is no background watchdog.

A finite process deadline starts after argument/environment construction, before FD
setup/spawn, and covers spawn/search and subsequent IO. It retains nanosecond
precision internally; poll rounds positive remaining time up to milliseconds.
On timeout, output overflow or post-launch system error, cleanup requests SIGKILL
and reaps the direct child. Pidfd readiness avoids periodic completion sleeps.
If pidfd signaling fails, cleanup can fall back to the still-unreaped numeric PID
under the host's no-external-reaper contract. Already-reaped children are not
signaled. Traced/stopped statuses do not release child ownership.

If cancellation is denied, cleanup performs a nonblocking wait and returns
SystemError with the errno. A nonzero unreaped_pid transfers responsibility for
termination/reaping to the caller **once**; repeated close/wait never transfers
it again. Failed reaping other than ECHILD likewise transfers ownership. This
field is not a durable identifier to signal after reaping.

A deadline is a cancellation budget, not a strict wall-clock bound: creation,
kernel/filesystem work and reaping after delivered signals can take longer.
Descendants are not supervised or killed. Descendants holding capture pipes can
cause TimedOut after the direct child exits; cleanup closes the remaining pipes.

## Bounds, platform and host responsibilities

| Bound | Limit |
| --- | --- |
| MAX_ARGUMENTS | 4,096 including executable |
| MAX_ARGUMENT_BYTES | 1,048,576 including one NUL per argument |
| MAX_ARGUMENT_LENGTH | 131,071 bytes per argument |
| MAX_PATH_BYTES | 4,095 each for executable and cwd |
| MAX_OUTPUT_BYTES | 1,048,576 per capture |
| MAX_INPUT_BYTES | 1,048,576 finite input or each incremental offer; no cumulative Stream input limit |
| MAX_TIMEOUT_MS | 86,400,000 (24 hours) for finite budgets; run minimum 1; explicit start-only 0 disables overall deadline |
| MAX_ENVIRONMENT_CHANGES | 256 total sets/removals |
| MAX_ENVIRONMENT_BYTES | 1,048,576 explicit change bytes including separators/NULs |
| MAX_ENVIRONMENT_KEY | 256 bytes per explicit key |
| Constructed environment | 4,096 resulting entries; 1 MiB including NULs; 131,071 bytes per complete record |
| Searched PATH | 16,384 bytes; 256 components; each candidate at most 4,095 bytes |

Custom inherited-environment construction inspects at most 4,096 inherited
entries; oversized inherited records are rejected even when removed. Unmodified
inheritance forwards the existing environment without imposing construction
bounds. The OS may impose a smaller combined argv/environment budget (E2BIG).
A single Child retains at most 2 MiB of captures plus its finite input copy.
A Pipeline retains three captures; see its separate bounds below.
Preparation scans/copies arguments/input, sorts explicit environment changes,
and zero-initializes configured captures. Native read scratch is 16 KiB; bounded
chunks keep both streams, input and cancellation responsive. These are payload
bounds, not exact allocator or RSS budgets.

Requires Linux kernel 5.3+ and glibc 2.34+; no silent fallback weakens descriptor
isolation. No Windows/macOS, PTYs, process groups, credential switching, arbitrary
pipeline graphs. Fd routing permits application-owned
live streams. See [comparison scope](COMPARISON.md).

Separate owners/buffers may run concurrently. The host must not externally reap
owned children, alter SIGCHLD disposition during operations, concurrently mutate
environment/cwd, or change borrowed source FDs while launching. SIGCHLD=SIG_IGN
and SA_NOCLDWAIT are rejected before spawn; custom handlers must not reap owned
children. Thread cancellation during operations is unsupported. Goose allocation
failure follows runtime policy; native allocation failures return SystemError/ENOMEM.
Native callers must provide valid pointer/length pairs; validation cannot make
arbitrary C pointers safe. The library is not an environment sanitizer or sandbox
and does not constrain a command's own interpretation of scripts/options.

## Managed two-stage pipelines

`Pipeline` owns exactly two commands connected by a blocking pipe. Use
`start_pipeline`, bounded `pipeline_step`, `wait_pipeline` and `close_pipeline`,
or synchronous `run_pipeline`. `PipelineStatus` preserves both stage outcomes
and actual launch flags; `pipeline_success` requires both stages to succeed.
Borrow captures with `pipeline_stdout`, `pipeline_first_stderr` and
`pipeline_second_stderr`. Views cannot survive a mutating owner operation.

Both stage timeouts must be equal and positive: one absolute deadline covers
preparation, launch and pumping. First input is Null or finite Bytes; first stdout
and second stdin are reserved for the connection. Both stderr streams and final
stdout are independently captured, each up to 1 MiB. Other routing modes are
rejected before launch. Retained captures plus finite input use at most 4 MiB payload;
`run_pipeline` additionally copies up to 3 MiB into caller buffers. Interstage
bytes pass through the kernel pipe without a library accumulation queue.

Ordinary nonzero exits and upstream SIGPIPE remain stage outcomes. Cancellation
requests both direct-child kills before either blocking wait. Each stage may
transfer one unreaped PID independently; callers must recover both while retaining
their execution scope. Downstream starts first and may perform side effects
before upstream launch fails. Descendants are not supervised; inherited capture
descriptors may keep a pipeline open until its deadline. See the full
[routing, ownership and failure contract](PIPELINES.md) before integrating.

## Bounded live output

Select `Output.Stream` explicitly in `start`, then call
`stream_step(child, out: u8[:], err: u8[:], timeout_ms = 0)` repeatedly. The return
values are `(Status, done, stdout_count, stderr_count)`. Counts describe only the
Stream bytes written into the corresponding caller buffer during this call.
Consume those prefixes before reusing the buffers. No payload or caller pointer
is retained by Stream between calls; `stdout(child)` and `stderr(child)` remain
capture-only accessors. Existing Capture defaults, limits and results do not change.

Each caller span is at most 1 MiB. A step reads at most 16 KiB from each managed
output and sends at most 16 KiB of finite stdin per poll iteration. Stream has no
cumulative byte limit: caller buffers and kernel pipes bound storage, while the
caller chooses any total-output/time policy. Existing capture allocations may
retain reusable capacity when a Child is reused. Limit options remain validated
but apply only to Capture. Finite copied stdin is still limited to 1 MiB.

Empty spans apply backpressure: that stream is not polled or read; other streams,
input and child control can progress. No extra-byte overflow probe is performed
for Stream. A full span may leave EOF unread until the next nonempty call.
Zero counts **do not mean EOF**. `done` means direct-child reaping and EOF on all
managed output pipes, or terminal cancellation/error cleanup. `try_wait` may reap
the child before its pending output is drained. Descendant-held pipes still need
an overall deadline or explicit close; there is no process-tree supervision.

Mixed Stream/Capture, inherited, null and FD routes are supported independently.
Pass an empty caller span for every non-Stream route. `stderr: Output.Stdout`
merges into stdout's route and returns zero stderr Stream bytes. Byte order is
preserved within a stream, with no ordering promise between separate streams.
On timeout, capture overflow or system failure, consume any returned Stream
prefixes before handling the error; cleanup may discard unread bytes. `close`
explicitly discards remaining I/O and retains the existing exceptional ownership
transfer contract.

`run` rejects Stream before launching. Use `stream_step` rather than legacy
`wait`/`step` for a streamed Child; those legacy Goose operations return
InvalidState without pumping. Signaling, status observation, close and owner reuse
remain available. Repeated terminal `stream_step` returns the final status and
zero Stream counts. Invalid spans, aliases or slice timeouts consume no bytes.
Slice budgets remain 0..24h, monotonic, without promising scheduler latency.
There is no background I/O: a slow caller intentionally backpressures the child,
and overall deadlines are enforced only while executing a library operation.
Application processing/writing errors must still reach close/exceptional cleanup.

## Bounded incremental stdin

Use `start` with `stdin: Input.Stream` and an empty `Options.input`. `run` rejects
this mode before launch. `Input.Bytes` keeps its existing copied finite payload
and automatic EOF. Both managed input modes use the same one-way Unix socket,
not a FIFO, with `MSG_NOSIGNAL`; parent SIGPIPE disposition/mask/pending state is
not changed.

`io_step(child, input: const u8[:], out: u8[:], err: u8[:], timeout_ms = 0)`
returns `Progress { status, done, input_accepted, stdout_count, stderr_count,
input_open }`. It requires an Input.Stream owner, including after EOF/completion.
All output routes remain available. Supply caller output spans only for Stream
routes; pass empty spans for Capture/Inherit/Null/Fd/merged stderr. Captures remain
available through existing accessors; count fields report only caller Stream spans.

Each offered input view is at most 1 MiB and remains stable during the call. No
input payload or caller pointer is retained afterward. One bounded poll iteration
can accept at most 16 KiB while draining both outputs and observing child state.
`input_accepted` counts bytes accepted by the **kernel**, not bytes consumed or
acknowledged by the command. Resubmit only the unaccepted suffix; the caller owns
any pending queue and total-input policy. Empty input is no offer, never EOF.
With no offer the input poll does not request POLLOUT, but still observes peer
closure; it cannot spin merely because the socket is writable.

`close_stdin` immediately closes the library's write endpoint. The child sees
EOF after already accepted bytes are drained. It sends no new input, waits for
nothing, does not drain output or reap the child, and does not reset its mode.
Repeated calls are harmless and preserve final error status. Continue io_step
until done, or use close for cancellation/discard. Legacy wait/step/stream_step
reject this owner before and after stdin closure, preventing accidental loss of
caller-driven input. Status observation, signaling and ordinary close remain valid.

`input_open` reports local ability to attempt another send. It may remain true
until peer closure is observed, and becomes false on explicit close, observed
closure, reaping or terminal cleanup. After it becomes false, even a nonempty
offer accepts zero bytes while output/reaping continue normally. Accepted bytes
can still be discarded by an early-exiting command. A repeated completed call
returns final status, done=true, zero per-call counts and input_open=false.

Counts include bytes accepted or delivered before a later timeout/system/capture
error in that call. Handle returned prefixes/counts before processing terminal
errors, including the existing explicit unreaped_pid transfer. None alone does
not imply completion; inspect done. Invalid input/state consumes no bytes and
changes no owner state, and returns zero counts/default flags; false input_open
in an invalid result is not evidence that a valid endpoint closed. Aliased input,
output, state, metadata and native result spans reject before I/O. Existing
monotonic overall/slice deadlines and direct-child-only ownership rules apply.
