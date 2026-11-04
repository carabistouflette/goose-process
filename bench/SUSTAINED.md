# Sustained consumer soak

This optional Linux harness extends the [short functional soak](SOAK.md) with a
monotonic duration budget, explicit warmup, finite output pressure and current
resource snapshots. It uses the same published Honk consumer and pinned Goose
compiler; it does not modify the consumer or goose-process runtime.

Each round runs one natural exit and one service through real Honk build/run.
The service writes 256 KiB to each independent stream. The controller drains
both streams during a two-second hold, then alternates direct SIGTERM and SIGINT
and checks complete payload delivery. Each service's locks must remain held
before cancellation and become available after cleanup. A session-validated
pidfd verifies the service identity is gone. The controller checks its settled
FD count and absence of unwaited children after every round.

```sh
taskset -c 0 python3 bench/soak_honk.py \
  --honk /absolute/honk/.build/honk --goose /absolute/goose \
  --consumer-source /absolute/honk \
  --consumer-revision 453aa53dc7b20622782423c64cc180097f653650 \
  --warmup-rounds 10 --duration-seconds 900 --hold-seconds 2 --pressure \
  --output .build/honk-sustained.json
```

Warmup rounds are retained and marked `measured: false`. The measured loop runs
for at least the requested duration, completing its last round. Build/setup,
warmup, final cleanup and identity checks are outside that duration. Per-command
waits remain bounded; this is not a hard whole-controller deadline.

Current `/proc/PID/status` VmRSS (KiB) and `/proc/PID/fd` counts are sampled for
the controller, Honk and the service at the end of each hold while their owned
identities remain unreaped. The controller is also sampled after cleanup.
These are current process snapshots, not Python's child-spawner high-water RSS.
They miss transient peaks and allocator details. Honk and the service are fresh
processes each round, so comparisons across rounds do not establish long-lived
Honk memory stability. The controller retains all raw samples in memory; its
RSS includes instrumentation and sample-list growth. No universal no-leak claim
or memory regression threshold is inferred from RSS.

The hold-phase drainer caps each stream at 1 MiB. The existing command and final
cancellation `communicate()` calls collect the finite trusted fixture's output;
they do not provide a byte cap for arbitrary or adversarial programs. The
service's pidfd is checked immediately before and after resource snapshots so
an unexpected early exit fails the run instead of attributing reused PID data.

Only direct signals are tested. Arbitrary descendants, terminal launch-window
classification, signal storms and continuous server uptime are outside scope.
The fixture's finite sleep loop is a safety backstop. Measurements on a shared
host are functional/resource observations, not isolated throughput benchmarks.
Exact consumer, executable, compiler, fixture and harness identities, platform,
CPU affinity, UTC timestamps, elapsed duration and all samples are retained.

## Recorded result: 2026-11-04

[All raw samples and identities](results/honk-sustained-2026-11-04.json) are retained.
The frozen harness source was commit
`b0497c4cba1f4f9bfa5b6b280ee3be07c99c888c`, with SHA-256
`d283263f298e907681d50a589bbd091986f7b95dfd640b6cfa1b206be74589fd`.
Consumer source was clean Honk
`453aa53dc7b20622782423c64cc180097f653650`. Executable, compiler and harness
hashes were verified before and after the run. Those identities and the
generated fixture hash are recorded in the JSON.

The measured interval began at 18:32:22 UTC and completed 901.2075 seconds after
10 warmup rounds. All 361 round pairs (722 executions) passed, including 351
measured pairs (702 executions). Measured services received 176 SIGTERM and
175 SIGINT cancellations. Each service delivered 262,144 bytes per stream.
All service identities were gone after cancellation and all locks released.
The controller retained no unwaited child after each round, and its settled FD
count was 4 at every checkpoint. During holds, measured descriptor counts were
controller 7, Honk 6 and service 3 throughout.

Measured current RSS snapshots ranged from 24,176 to 25,556 KiB for the
controller, 8,336 to 8,816 KiB for fresh Honk processes, and 2,444 to 2,664 KiB
for fresh services. These observed ranges are not resource limits. Settled
controller RSS medians for the first and last ten measured pairs were 24,178
and 25,546 KiB respectively. The controller retains samples and Python allocator
state; this increase is recorded without attributing it solely to either
instrumentation or a leak. Fresh-process RSS comparisons cannot establish
long-lived consumer memory stability.

The host was shared Linux x86-64/glibc, with Python 3.14.9 and CPU0 affinity
inherited by children. No exclusive-core or isolated performance claim is made.
This was one sustained functional series, not multiple independent repeats.
No production runtime defect was reproduced; no patch release or consumer repin
is justified by this result.

Two independent harness reviews preceded the run. The safety reviewer identified
an expired numeric-PID sampling race; checks of the anchored pidfd now bracket
resource reads. The measurement reviewer corrected the documented order of
payload validation and cancellation. Pressure and duration/warmup preflights
passed, as did `make check`. The optional sustained soak is local validation;
it is not executed by the normal hosted CI workflow.
