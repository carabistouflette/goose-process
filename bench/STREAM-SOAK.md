# Sustained resident streaming validation, 2026-11-05

One resident Goose consumer completed **19,879 measured cases over
300.054586994 seconds**, after 12 warmups. All cases passed. At every quiescent
checkpoint it had three descriptors and no remaining child. No library runtime
patch or application repin was warranted by these results.

This is shared-host functional/resource evidence, not a general speed ranking,
peak-memory measurement, stability guarantee or proof of absence of bugs.

## Reproduce

```sh
make setup
# Commit any local source edits before recording a measurement.
taskset -c 0 python3 bench/stream_soak.py --goose .toolchain/goose/build/goose --cc gcc --seconds 300
# Separate instrumented profiles; cancellation is excluded from these two tools.
taskset -c 0 python3 bench/stream_profile.py --finite-only --tool valgrind --tool callgrind --output .build/stream-profile-finite-current.json
```

Python, GCC, the pinned Goose compiler, Linux/glibc, strace, Valgrind and
`callgrind_annotate` are host tools. Compilation/profiling is separate from the
measured region. Normal tests run only a one-second smoke, not a five-minute CI
timing threshold. The script can accept 1..3600 seconds, subject to explicit
protocol output caps. It stops after the requested duration at a completed-case
handshake, preserving the last case rather than truncating it.

## Source and raw evidence

- [Complete soak JSON](results/stream-soak-2026-11-05.json), SHA256
  `fcd443c05871098077bf3845712f0d5c31e557ab3a93c985f2c1322e4342a9e5`.
- Measured clean source: `51b5226a3c080e34dd4192b7e3e823eff745dc57`. Source,
  compiler, controller, fixture and executable identities were checked before/
  after. Later commits add profile-only routing, correct profiling fixture copy
  permissions, add CI smoke/retention assertions, and retain results/docs; they
  do not change library runtime source or the measured executable.
- CPU0 affinity on Intel Core Ultra 7 258V, GCC 16.2.1, Python 3.14.9,
  Linux 7.2.11/glibc 2.44; exact flags/versions/load are in JSON. CPU affinity
  provides no isolation. No concurrent heavy local job was launched during this
  series; other host activity was not controlled.

The JSON retains every case record and raw protocol stdout, all warmups and
resource snapshots. Payloads are validated byte-for-byte by the Git-matched
driver; payload bytes themselves are not logged for independent replay. Fixture
bytes are reconstructible from source and lengths. No samples were filtered.
The UTC envelope includes warmup/shutdown; the measured duration is monotonic.

## Workloads and timing scope

A single noncopyable Child is reused. Seed 20261105 chooses views of 1, 7, 255,
4093 and 16384 bytes independently while draining. The large dual-stream case
uses position-dependent binary oracles and pauses 1ms after every 32 steps to
exercise a slower reader. Two full six-case cycles warm every case before the
resident baseline. Ordinary and mixed output captures keep their existing limits.

| Case | Measured count | Median ms | Empirical p95 ms | Maximum ms |
| --- | ---: | ---: | ---: | ---: |
| Two binary 3 MiB streams, slow reader | 3,314 | 72.133 | 91.409 | 378.538 |
| Binary 1 MiB stdin roundtrip | 3,313 | 4.432 | 8.943 | 33.885 |
| Flood followed by TERM | 3,313 | 0.976 | 4.263 | 26.422 |
| TERM near exit / observed reaping | 3,313 | 2.271 | 3.715 | 23.324 |
| Early stdin closure | 3,313 | 1.272 | 4.261 | 25.426 |
| Stream stdout / capture stderr | 3,313 | 0.835 | 3.742 | 12.119 |

Medians use `statistics.median`; p95 selects sorted index `floor((n-1)*0.95)`.
These are case completion times including public calls, byte assertions,
explicit pauses, signal operations and close. They are not pure-library latency
or scheduling guarantees. Controller handshakes/probes are outside case timers:
case times sum to 287.736855797s versus the measured envelope's 300.054586994s.
Flood/TERM cases check received prefixes and final status; cancellation makes
their total byte counts variable.

Near-exit cases recorded 1,652 already-reaped observations (InvalidState from
terminate), 1,621 TERM exits and 40 normal exits after an accepted terminate.
Signal success does not guarantee the signal won. No ESRCH outcome occurred in
this run; that accepted branch is not claimed as exercised. No unrelated PID is
signaled, and the controller never reaps a library-owned child while the driver
is alive.

## Resource observations

The driver prints one record after cleanup and blocks for the next command.
Only then does the controller inspect that driver's `/proc` data. This measures
the resident consumer, not a Python spawner's high-water RSS.

| Quiescent observation | Warm baseline | Measured snapshots | Final |
| --- | ---: | ---: | ---: |
| Descriptors | 3 | 3 in every case | 3 |
| Remaining children | 0 | 0 in every case | 0 |
| RSS, KiB | 5,712 | 5,712 in all 3,313 cycle snapshots | 5,712 |
| Mapping count | 73 | 73 | 73 |
| VmSize, KiB | 29,378,792 | 29,378,792 | 29,378,792 |

There are also two warmup memory snapshots. VmSize is virtual address space,
including the Goose runtime's reservations; it is not physical resident memory.
These quiescent observations show no growth here, but omit active-case peaks,
child memory, allocator accounting and longer/multithreaded workloads.

## Separate profiling and retained failures

[Full profile attempt](results/stream-profile-full-2026-11-05.json): parent-only
strace completed all 12 cases. It counted 6,221 selected syscalls, including
3,742 reads, 2,216 polls and 12 waits. Its timing is instrumented and is not
used for the soak's latency results. Child executable work is not traced.

The same full attempt **failed under Memcheck** at the cancellation case:
Valgrind 3.25.1 reported unhandled syscall 424 (`pidfd_send_signal`). A
[plain-libc control](results/pidfd-profile-control-2026-11-05.json) independently
returned success natively and ENOSYS/38 under Memcheck, reaping its exclusively
owned child in both cases. The failed full run's controller also found/reaped an
adopted fixture. Its zero-memory-error summary is not a completed-run pass, and
no library signaling workaround was introduced.

The [first finite-only attempt](results/stream-profile-finite-first-2026-11-05.json)
also failed: a profiling helper copy lost its executable permission. This harness
bug was corrected by preserving source file metadata. It did not affect the
completed soak. Both failures and normalized logs remain visible.

[Corrected finite profiles](results/stream-profile-finite-2026-11-05.json) use a
separate executable at source `54a6679919f2b11ac0adcb78a35c8f7853b69b7f`, with
the same compiler/helper bytes. Twelve finite cases (kinds 0, 1, 4, 5) completed
under both Memcheck and Callgrind; they exclude signal/cancellation paths.
Memcheck reported zero errors and zero definitely/indirectly/possibly lost heap
bytes; 66,200 runtime bytes remained reachable. This does not fully account for
Goose's mmap-backed storage or prove all memory behavior.

Callgrind counted 404,034,184 instructions, with 98.92% attributed to generated
`main_g.constprop.0`. That symbol combines inlined wrapper/harness/byte-validation
work, so it does not isolate a library CPU bottleneck or justify an optimization.
Profiles retain source/binary/tool identities, raw-log hashes and normalized
log/annotation text. Only the checkout prefix is replaced with `<source>`.

Independent design/security, harness and data reviews checked these contracts,
cleanup and records. Concrete harness issues fixed were line-buffer reuse,
trailing failure-output retention, and profile fixture permissions. These checks
supplement the normal/debug/sanitizer/fault suites; they do not replace them.
