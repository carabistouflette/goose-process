# Incremental duplex validation

## Scope and acceptance criteria

This milestone follows the reviewed Input.Stream publication at
`68fe55bf8c0d95260d15c507a40fba6dd726502f`. It adds evidence and developer
fixtures, rather than a new runtime policy or application dependency pin.

The approved work is:

1. Exercise long-lived incremental workers, independent backpressure, slow
   production/consumption, early peer closure, cancellation, deadlines and Child
   reuse in a resident consumer. Validate binary prefixes, exact successful totals
   and kernel acceptance separately from child consumption.
2. After warmup, observe the resident consumer only after each completed case
   has drained/reaped/closed and is awaiting the next controller command.
   Require stable descriptor count and no direct child. Retain RSS, mappings and
   virtual size without asserting a universal memory or stability guarantee.
3. Compare Rust, Go, Python and Goose with the same absolute C worker executable,
   framing, binary bytes, request count, output oracles, explicit EOF and cleanup.
   Include byte validation and adapter safety work in timing. Describe differing
   policies and retain all raw samples, compiler/source/binary identities,
   shuffled order, warmups and shared-host limitations.
4. Fix demonstrated defects or bottlenecks with focused commits and independent
   review. Do not infer a runtime patch from a small timing difference or claim
   general speed, memory, safety or feature superiority.

## Resident workload

```sh
make setup
# Commit sources before recording a measurement; affinity is not isolation.
taskset -c 0 python3 bench/duplex_soak.py --goose .toolchain/goose/build/goose --seconds 300
```

One resident Goose process reuses one Child across a six-case cycle. Two full
cycles warm it before measurement. The controller finishes a complete cycle
after the requested duration, retaining all records. The source fixes the seed
and independently varies input, stdout and stderr views; no payload pointer is
retained across an io_step call. Empty offers and temporarily empty output spans
resume actual progress on subsequent calls.

| Case | Required outcome |
| --- | --- |
| 400 framed worker requests | 1,149,760 accepted/echoed bytes, 1,600 ack bytes, explicit/repeated EOF and exit0 |
| Output-before-input duplex burst | Both 256KiB prefixes checked; 2MiB incremental input echoed; slow feed/drain and exit0 |
| Early input closure | `closed` output; observed input closure persists with zero further acceptance; owned TERM and reap |
| Active cat cancellation | Actual echo before owned TERM; returned binary prefix matches accepted input; accepted bytes may be discarded |
| Open-input deadline | Idle helper, 50ms library budget; TimedOut and cleanup; no claim child readiness was observed |
| Mixed stream/capture overflow | Streamed stdout prefix checked, stderr capture stops at127 matching bytes, OutputLimit and cleanup |

Every case checks completed-operation repetition: same final status, zero counts,
closed input and no transferred PID. Close precedes the next case. Unexpected
exceptional ownership fails the fixture; the standalone controller first kills
its private, unreaped process group, reaps its leader, then reaps adopted orphan
fixtures. It never externally waits a living driver's library-owned child.

Diagnostic collection remains capped during trailing-output/failure cleanup.
Controller fixtures check diagnostic retention, a hanging descendant, stdout and
stderr overflow and descriptor equality. The inherited ownership controller is
a standalone single-threaded subreaper, not an application signal policy.

Per-case wall timers include public calls, byte oracles, close, and intentional
pauses. Controller handshakes and /proc probes are outside case timers. Quiescent
RSS excludes active peaks, children, allocator accounting and other workloads.
The report retains source/compiler/fixture identities before and after, every
record, UTC and monotonic envelopes, and controller cleanup results.

## Matched worker comparison

See [the adapters and reproduction method](../bench/duplex_compare/README.md).
The fixed successful protocol uses128 sequential request/response exchanges per
child, with31-byte and4096-byte payload cases. It is neither raw spawn cost nor a
per-request latency distribution. All adapters bound the expected byte counts
and use30-second budgets, but transport, language runtimes, signal/descriptor
setup, polling/reader orchestration and exceptional cleanup differ. No complete
safety-contract parity is asserted.

## Results and independent review

Measurements and final review conclusions will be retained after clean-source
validation; no timing or sustained-run conclusion is claimed by this plan.
