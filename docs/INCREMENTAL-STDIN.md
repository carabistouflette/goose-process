# Incremental stdin and a framed worker

Use `Input.Stream` to send chunks while reading a child's output. This API is
on main, not v0.1.0. See the [full contract](API.md#bounded-incremental-stdin)
for buffer lifetimes and status handling.

```goose
let launched = process::start(command, process::Options {
    stdout: process::Output.Stream, stderr: process::Output.Stream,
    stdin: process::Input.Stream,
}, child);
// Check launched, then offer unaccepted suffixes while consuming both outputs.
let progress = process::io_step(child, offered, out, err, 20);
// Account for input_accepted and output prefixes even on terminal errors.
// When all wanted input is accepted, explicitly close stdin and continue pumping.
```

There is no library payload queue. Each call borrows at most 1 MiB, accepts at
most 16 KiB, and retains no input pointer. Caller buffers remain stable during
the call and can be reused afterward. The caller must keep its unaccepted suffix
and inspect input_open, done and status. An empty offer does not imply EOF.
Closing the write endpoint sends EOF after kernel-accepted bytes drain; it does
not acknowledge consumption or wait for the worker. See CALLERS.md for explicit
exceptional cleanup ownership rather than treating this sketch as a full runner.

## Run the worker demonstration

```sh
make setup helpers
.toolchain/goose/build/goose build examples/worker.goose --cc gcc -O2 -o .build/worker-client
.build/worker-client
```

`examples/worker.c` is a fixed framed echo worker: four-byte big-endian length,
at most 4096 payload bytes; stdout echoes the frame, stderr emits `ack\0`.
Truncated/oversized frames fail with a nonzero exit. It is not an executable
command protocol or general worker framework. The Goose client sends 400
variable-size frames (including empty and binary payloads), validates every
response and acknowledgment with fixed buffers, then explicitly closes stdin
and reaps the worker. The cumulative input/stdout is 1,149,760 bytes, exceeding
the finite Bytes limit, with no whole-log/input queue in the client. It prints
only after normal cleanup; its assertions are fixed-fixture test oracles, not a
complete production recovery policy for hostile workers or transferred PIDs.

## Regression coverage

`tests/stdin.goose` verifies a 3 MiB binary roundtrip through changing remaining
views, >socket-capacity duplex input while the child fills both outputs before
reading, empty offers, explicit/repeated EOF, mid-process peer closure, discarded
unaccepted bytes, capture-only/mixed routing and overflow, invalid offers/slices,
reaping before output drain, terminal repetition and owner reuse. Legacy pumping
methods reject new-mode owners; synchronous run rejects before launch.
Borrowed/merged output routing and compile-time rejection of capture views held
across io_step are also covered.

`tests/stdin.c` injects a clock failure immediately after a real successful send:
accepted bytes remain reported, cleanup is terminal, waitpid proves ECHILD and
descriptors return to baseline. A targeted denied terminal wait also transfers
the PID exactly once while preserving accepted bytes; the local recovery owner
then proves terminal reaping/ECHILD. A retryable EAGAIN reports zero acceptance,
and a following empty offer proves no pending payload was retained. It also
checks result/input/output/state aliases and input poll events=0 for an empty offer.
MSG_NOSIGNAL is asserted at
the send seam. Existing finite-input signal-state, EINTR deadlines, lifecycle and
denied-cleanup transfer tests remain active. The intercepted-spawn fuzzer now
exercises start-only input routing without executing mutated paths.


[Historical timing and validation](INCREMENTAL-STDIN-HISTORY.md) records earlier
implementations. See [benchmark source scope](../bench/CURRENTNESS.md).
