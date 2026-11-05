# Bounded framed-worker comparison

This track compares four implementations of one successful interactive protocol.
It measures a whole checked worker session, not pure spawn time, individual
request latency, whole-API feature parity or a general language/safety ranking.

From a standalone Linux checkout, after `make setup`, with GCC, installed Rust,
Go and Python 3.11+:

```sh
python3 bench/duplex_compare/measure.py --build-only
python3 bench/duplex_compare/check.py
# Optional actual30-second timeout checks, before first response and after EOF:
taskset -c 2 python3 bench/duplex_compare/check.py --deadlines
# Commit sources first. Affinity is inherited; it does not isolate the host.
taskset -c 0 python3 bench/duplex_compare/measure.py --rounds 9 --calls 5
```

The scheduler downloads no packages. Generated binaries/default results remain
in ignored `.build/duplex-compare*`. Go uses its local toolchain with module
downloads disabled; Rust invokes the installed compiler identified by rustup's
sysroot, preserving the distinction between a rustup proxy and the compiler.
Compiler versions and hashes are captured from actual tools, rather than inferred
from the documentation versions below. The Rust adapter calls glibc's
`pidfd_open` symbol and therefore additionally requires **glibc 2.36+**;
this does not change the library's glibc 2.34 requirement. The symbol was added
in the [glibc 2.36 release](https://lists.gnu.org/archive/html/info-gnu/2022-08/msg00000.html).

## Shared workload

Every driver launches the same absolute executable, compiled unmodified from
`examples/worker.c`, and exchanges 128 sequential request/response frames through
one child. Payload size is either 31 or 4096 bytes. A frame has a four-byte
big-endian length and payload byte `(index*17 + index/251) % 256`. The same fixed
frame is constructed before the timer and reused; the sender waits for its
complete echo and `ack\0` on stderr before issuing the next request.

| Payload bytes | Input/stdout bytes per session | Stderr bytes |
| ---: | ---: | ---: |
| 31 | 4,480 | 512 |
| 4,096 | 524,800 | 512 |

All implementations use a frame of at most 4100 bytes and stdout/stderr reads of
at most 257/3 bytes. They compare every returned header, payload and acknowledgment
byte, reject per-request excess output or early EOF, explicitly close stdin,
reject trailing output and require final exit 0. Captures do not grow with input.
Each adapter enforces a 30-second monotonic/context budget and owns ordinary
failure cleanup. As with the library, process creation and kernel scheduling can
delay deadline observation; this is not a strict wall-clock completion guarantee.

These are fixed-fixture adapters, not general hostile-command runners. The
controller starts each driver in a private session, has an independent
45-second timeout, and is a subreaper. It kills its still-owned private process
group on exceptional driver failure and treats any adopted worker as a failed
validation, after cleanup. It never reaps a living driver's library-owned child.
The Goose driver closes Child before failing an ordinary protocol oracle;
transferred-PID recovery under denied cleanup remains the controller's test
boundary rather than a production policy implemented by these benchmark drivers.

## Different implementations and policies

- Goose uses `Input.Stream`, two `Output.Stream` routes, `io_step`, explicit
  `close_stdin` and `close`; it retains its socket stdin, validation, signal
  resets, descriptor isolation, pidfd lifecycle and direct-child cleanup policy.
- Rust uses `std::process::Command`/`Child`, real stdin/stdout/stderr pipes and a
  small Linux-only nonblocking `poll`/`fcntl` adapter. An owned pidfd wakes the
  final wait after output EOF without inserting a fixed idle delay. The driver
  explicitly kills/waits an exclusively owned, unreaped Child on error; it never
  calls kill after successful reaping. This is a custom deadline/bounded-I/O
  adapter around Rust std, not a claim that std exposes the Goose API.
- Go uses `exec.CommandContext`, its three pipe methods and two fixed-buffer
  reader goroutines with explicit request permissions and one-record result
  channels. Reads detect a span exceeding the current response remainder. It waits for readers to
  complete before `Cmd.Wait`, and cancels/kills/waits on error. Goroutine/runtime
  allocation and scheduling are part of this route.
- Python uses binary `Popen` pipes, nonblocking descriptors and selectors,
  checking the remaining monotonic deadline and killing/waiting on failure.
  Each read and slice allocation is bounded; it does not use an unbounded
  `communicate` capture in the timed driver.

Socket versus pipe semantics, spawn/FD/signal setup, allocation and exceptional
cleanup behavior differ. Similar successful bounds/deadlines are not proof of
equivalent safety guarantees; only the specified protocol is matched. Ordinary
default, ignored, custom or inherited signal semantics are not normalized across
these baselines by this benchmark. No process-tree supervision claim is made.

Official API references were checked on 2026-11-05:
[Rust Child](https://doc.rust-lang.org/std/process/struct.Child.html),
[Go os/exec](https://pkg.go.dev/os/exec), and
[Python subprocess](https://docs.python.org/3/library/subprocess.html).
Those floating pages identified Rust std 1.99.1, Go 1.27.3 and Python 3.15.2;
the actual installed preflight tools were Rust 1.101.0-nightly, Go
1.27.3-X:nodwarf5 and Python 3.14.9. Results must use the actual recorded versions.

## Timing and provenance

Driver wall time starts immediately before worker setup/launch and ends after
all 128 exchanges, exact comparisons, explicit EOF, output drain, reaping and
ordinary local descriptor cleanup. Driver startup/imports, argument parsing,
fixed-frame construction and compilation are excluded. Session wall time includes
runtime allocations, poll/select/channel setup and all active protocol work.
No timing instrumentation is added to the worker.

Each round shuffles the eight language/payload cells with seed 20261105. Each
cell retains one separate warmup and five measured invocations by default.
Every invocation starts a new driver process; warmup does not keep its language
runtime resident. A session itself keeps one child for all 128 requests. Raw JSON
retains every record/output/status/order, compiler/platform/affinity/load, flags,
Git source revision/dirty state and source/compiler/driver/worker hashes. It
checks hashes and Git identity before/after; ordinary timing requires clean source.
A failure aborts the series and retains the failed driver output and invocation
context in its error record rather than skipping it. Watchdog failures retain
the partial stdout/stderr observed when the timeout occurred, after private-group
cleanup; they do not claim to reconstruct bytes produced beyond that observation.
Successful and rejected-fixture probes are
separate from measured samples.

Auxiliary outer wall and process-tree user/system CPU include driver startup and
imports as well as the waited worker. They cannot be interpreted as isolated
library CPU; this track makes no resident-memory/RSS claim. Summaries should use
the median of per-round cell medians and disclose round ranges and host activity.
CPU affinity does not provide exclusive cores, temperature/frequency control or
portable speed guarantees. Timing differences require interpretation of these
contracts and measurement noise before an optimization is justified.

`check.py` runs all eight successful combinations, then rejects four actual
worker variants for each language: early EOF, corrupt stdout and excess output
after explicit EOF, and all responses frontloaded before requests. All sixteen
must fail without leaving an adopted child. A separate watchdog fixture proves
partial stdout/stderr retention after timeout cleanup.
It does not establish generic failure-policy parity or prove the absence of bugs.

`--deadlines` additionally retains eight actual timeout probes with source/tool/
binary hashes, affinity and timestamps. Its concurrent sessions are functional
deadline/cleanup checks, not a latency or throughput comparison.
