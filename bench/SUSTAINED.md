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
