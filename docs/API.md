# API and execution contract

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
| `timeout_ms` | `30000` | Positive monotonic cancellation budget |
| `output_limit` | `65536` | Default maximum retained bytes per captured stream |
| `stdout`, `stderr` | `Output.Default` | Independently select output routing |
| `stdin` | `Input.Null` | Select stdin routing |
| `input` | `""` | Finite binary input, valid only with `Input.Bytes` |
| `stdout_fd`, `stderr_fd`, `stdin_fd` | `-1` | Borrowed source FD for the corresponding Fd mode |
| `stdout_limit`, `stderr_limit` | `-1` | Override output_limit; -1 uses the default |
| `clear_environment` | `false` | Start with an empty environment instead of inheriting |
| `environment` | empty | Slice of `EnvOverride { key, value, remove }` |
| `search_path` | `false` | Explicitly enable PATH search and relative executables |

`Output` supports Default, Capture, Inherit, Null, Fd, and **stderr-only** Stdout.
Stdout merges stderr into the child's already-routed stdout. A merged capture
uses only the stdout limit and returns an empty stderr buffer. Separate captures
have no combined ordering. Null discards output through `/dev/null`.

`Input` supports Null (`/dev/null`, read-only), Inherit, Bytes, and Fd. Bytes
copies at most 1 MiB before execution and sends it while draining both captures.
It uses a **one-way Unix socket byte stream**, not a FIFO, to suppress write-side
SIGPIPE with `MSG_NOSIGNAL` without changing the caller's signal state. Empty
input delivers EOF. Early child-side closure is normal; unsent input is discarded.
For programs requiring a real pipe, terminal, file or other descriptor type, use
Fd. The library does not feed or manage a caller-supplied stdin FD.

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

Declare `var child = process::Child { .. };` as a local or global owner.
`Child` is a compiler-enforced noncopyable root; unannotated aliases borrow it.
It cannot be stored by value in arrays/other structs or sent to another thread.
Its underscore fields and the native ABI are implementation details, not public
API. Do not modify them directly. There is **no automatic destructor or background
watchdog**: always finish an active child with wait, repeated step, or close.

| Function | Contract |
| --- | --- |
| `start(args, options, child)` | Launch; copy finite input and allocate captures; reject an active owner |
| `step(child, timeout_ms = 0)` | Feed/drain at most one bounded IO iteration; return Status and completion flag |
| `wait(child)` | Feed/drain to EOF/completion or cancellation |
| `try_wait(child)` | Nonblocking status observation/reaping; return Status and whether direct child reaped |
| `send_signal(child, signal)` | Signal unreaped direct child by pidfd; zero tests signal permission/existence |
| `terminate(child)`, `kill(child)` | Send SIGTERM or SIGKILL; still call wait/step/close |
| `close(child)` | Discard remaining IO, cancel/reap active child; idempotent |
| `id(child)` | Observational PID while owner active; -1 otherwise; never use it to signal a recycled PID |
| `stdout(child)`, `stderr(child)` | Read-only views of collected prefixes; consume/copy before a mutating operation |

A step timeout ranges from 0 to 24 hours; zero polls once without waiting.
Its slice uses an absolute monotonic deadline even under EINTR. The overall
process deadline is checked during IO operations, not while an idle Child is
held. try_wait does **not** feed input or drain output; a child blocked on IO may
never finish through try_wait alone. Even when try_wait reaps the direct child,
call wait/step to collect remaining captures or close to discard them. A completed
wait may be repeated. An unsuccessful start may replace previous captures;
rejection of an already-active owner leaves it unchanged. An owner can be reused
after completion/close. Views cannot outlive
operations that resize captures; the Goose checker enforces this lifetime rule.

The process deadline starts after argument/environment construction, before FD
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
| MAX_INPUT_BYTES | 1,048,576 finite input |
| MAX_TIMEOUT_MS | 86,400,000 (24 hours); process minimum 1 |
| MAX_ENVIRONMENT_CHANGES | 256 total sets/removals |
| MAX_ENVIRONMENT_BYTES | 1,048,576 explicit change bytes including separators/NULs |
| MAX_ENVIRONMENT_KEY | 256 bytes per explicit key |
| Constructed environment | 4,096 resulting entries; 1 MiB including NULs; 131,071 bytes per complete record |
| Searched PATH | 16,384 bytes; 256 components; each candidate at most 4,095 bytes |

Custom inherited-environment construction inspects at most 4,096 inherited
entries; oversized inherited records are rejected even when removed. Unmodified
inheritance forwards the existing environment without imposing construction
bounds. The OS may impose a smaller combined argv/environment budget (E2BIG).
Logical captures total at most 2 MiB; Child also owns a finite input copy.
Preparation scans/copies arguments/input, sorts explicit environment changes,
and zero-initializes configured captures. Native read scratch is 16 KiB; bounded
chunks keep both streams, input and cancellation responsive. These are payload
bounds, not exact allocator or RSS budgets.

Requires Linux kernel 5.3+ and glibc 2.34+; no silent fallback weakens descriptor
isolation. No Windows/macOS, PTYs, process groups, credential switching, pipeline
builder or arbitrary incremental stdin API. Fd routing permits application-owned
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
