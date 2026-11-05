# Incremental duplex validation

## Scope and acceptance criteria

This milestone follows the reviewed Input.Stream publication at
`68fe55bf8c0d95260d15c507a40fba6dd726502f`. It adds evidence and developer
fixtures, rather than a new runtime policy or application dependency pin.

The validation criteria are:

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
| Output-before-input duplex burst | Both 256 KiB prefixes checked; 2 MiB incremental input echoed; slow feed/drain and exit0 |
| Early input closure | `closed` output; observed input closure persists with zero further acceptance; owned TERM and reap |
| Active cat cancellation | Actual echo before owned TERM; returned binary prefix matches accepted input; accepted bytes may be discarded |
| Open-input deadline | Idle helper, 50 ms library budget; TimedOut and cleanup; no claim child readiness was observed |
| Mixed stream/capture overflow | Streamed stdout prefix checked, stderr capture stops at 127 matching bytes, OutputLimit and cleanup |

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
The fixed successful protocol uses 128 sequential request/response exchanges per
child, with 31-byte and 4096-byte payload cases. It is neither raw spawn cost nor a
per-request latency distribution. All adapters bound the expected byte counts
and use 30-second budgets, but transport, language runtimes, signal/descriptor
setup, polling/reader orchestration and exceptional cleanup differ. No complete
safety-contract parity is asserted.

## Results and independent review

The [complete resident record](../bench/results/duplex-soak-2026-11-05.json)
contains **2,466 measured cases over 300.168979434 seconds**, plus 12 warmups.
Each of the six kinds ran 411 measured times. Measured clean source is
`a91e10f7b8337433ba904accf291c696e9d53ba8`; source/compiler/fixture/binary
hashes remained unchanged. All cases passed. Every measured quiescent checkpoint
had three descriptors, no direct child, RSS 2,840 KiB, 79 mappings and VmSize
33,575,152 KiB. Controller descriptors stayed four; no adopted fixture required
cleanup. These observations omit active peaks, child memory and other workloads.
VmSize includes runtime address-space reservations, not physical resident memory.

| Resident case | Median ms | Empirical p95 ms | Maximum ms |
| --- | ---: | ---: | ---: |
| 400 framed requests | 616.481 | 796.855 | 1,154.584 |
| 2 MiB duplex burst | 22.152 | 26.452 | 2,288.997 |
| Early peer close / TERM | 0.932 | 1.308 | 17.353 |
| Active cat / TERM | 20.768 | 21.565 | 44.876 |
| Open-input deadline | 51.124 | 51.600 | 59.198 |
| Mixed capture overflow | 1.199 | 1.462 | 8.243 |

p95 selects sorted index floor((n-1)*0.95). All outliers are retained, including
the 2.289-second burst at index 2,425 and 1.024-second burst at index 2,419.
Case timers sum to 298.502469243 seconds; controller probes/handshakes explain
the remaining measured envelope. Neither these case latencies nor the fixed
50 ms timeout fixture establish a scheduling or strict completion guarantee.

Both the resident series and the separate comparison used CPU0 on a shared
Intel Core Ultra 7 258V host, Linux 7.2.11/glibc 2.44 and GCC 16.2.1. Affinity was
inherited by fixture children; no exclusive isolation was claimed. Separate
eight-way deadline checks on CPU2 overlapped part of the soak; focused review
fixtures were unpinned. External activity, temperature/frequency and SMT effects
were uncontrolled. Those early development deadline checks were not the final
revision's evidence; the final retained checks below replace that limited record.

The [final comparison and profiles](../bench/duplex_compare/RESULTS.md) retain
360 measured sessions and 72 warmups. The [actual deadline record](../bench/results/duplex-deadlines-2026-11-05.json)
at `1395b4f8a3c0e6895e27dee747b2bfd387cc77f9` checks all four adapters with
idle workers before the first response and after protocol EOF. All eight
30-second probes rejected the worker and cleaned up without an adopted child;
their observed completion was 30.010–30.185 seconds, including driver startup.
They have separate CPU2 affinity, UTC, source/tool/binary hashes and diagnostics.
Concurrent probes are functional checks, not throughput or latency rankings.

Independent reviews were separated:

- Design and controller/security: no blocking lifecycle finding. Two coverage
  suggestions now enforce continued zero acceptance after peer closure and
  final controller descriptor equality.
- Standards: no actionable documented-standard violation or substantial smell.
- Spec: three findings fixed before comparison timing: Go response readers
  could prefetch future frames; its timer excluded channel setup; watchdog
  exceptions lost partial diagnostics. Reader permissions/bounds, timer placement
  and failed-invocation context are now checked. Eight successful combinations,
  sixteen bad-worker rejections and a watchdog-retention fixture passed.
- Evidence/provenance: independently recomputed the resident results and initial
  comparison, matched recorded source/tool/binary identities and noted the
  development deadline record's limits. Final-span comparison/profile conclusions
  are reported separately with their actual source identities.

Full normal GCC and Clang interpreter/release/debug/native suites passed, as did
the new debug/no-BCE driver cycles, bounded controller failure fixtures and
optional comparator checks. Hosted CI remains an exact-head publication gate.
The library runtime, native sources, compiler pin, release tags and application
dependency pins are unchanged from 68fe55b. No runtime patch or general speed,
memory, safety or stability claim follows from this evidence.
