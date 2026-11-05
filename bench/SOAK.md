# Controlled Honk service soak

**Historical study.** Results and unchanged-source statements below apply to
the named revisions, not current main. Reproduce with those source, compiler and
dependency versions. See [current benchmark scope](CURRENTNESS.md).

Post-release validation on 2026-11-04 used public Honk main
[`453aa53dc7b20622782423c64cc180097f653650`](https://github.com/carabistouflette/honk/commit/453aa53dc7b20622782423c64cc180097f653650),
with [GCC/Clang CI](https://github.com/carabistouflette/honk/actions/runs/41860574134).
The independent checkout and cache were isolated from the shared Honk workspace.

[The harness](soak_honk.py) creates and builds a real Goose project, then runs
12 natural exits and 12 bounded service/cancellation scenarios through Honk's
actual build/run commands. Services alternate direct SIGTERM and SIGINT. Project
and cache locks must remain held while the service is running and become
available after Honk returns. A session-validated pidfd checks that the service
identity is gone. Controller descriptors and unwaited children are checked.
All 24 scenarios passed; no runtime defect was reproduced in this workload.

[Raw results](results/honk-soak-2026-11-04.json) retain the exact consumer commit,
Honk/compiler/harness/fixture hashes, per-run outcomes and controller FD counts.
Source cleanliness, source revision and executable/script hashes are checked
before and after. The fixture flushes readiness before publishing an atomic PID
marker; the controller validates the opened identity belongs to its private
session before enabling cleanup signaling. Failure cleanup gives Honk time to
reap and uses bounded escalation only within the still-owned private session.
The standalone, single-threaded controller normalizes SIGCHLD to its default
before spawning; it has no external reaper, so leader PID reservations remain
held until its own wait/communication completes.

Independent reviewers found failure-path issues in the initial, unpublished
harness: unvalidated numeric markers, direct-only command timeout cleanup,
readiness ordering and cleanup ordering/error handling. These were fixed before
recording this result. Controlled probes also rejected a different-session PID
without signaling it and verified owner cleanup after an injected pidfd denial.
These are harness corrections; no production-library fix or patch release was
justified by this run.

Reproduce after building the exact public Honk checkout, using the Goose compiler
pinned in GOOSE_REVISION:

```sh
python3 bench/soak_honk.py --honk /absolute/honk/.build/honk \
  --goose /absolute/goose --consumer-source /absolute/honk \
  --consumer-revision 453aa53dc7b20622782423c64cc180097f653650 \
  --rounds 12 --output .build/honk-soak.json
```

This is a short controlled functional soak, not hours/days of service uptime,
a throughput or memory benchmark, or proof of arbitrary descendant cleanup.
The actual fixture has a finite loop as a safety backstop, and controller waits
are bounded. It tests direct signals; terminal launch-window ambiguity and
arbitrary signal storms are outside this workload. Existing native/public and
Honk-specific terminal/recovery tests cover different scenarios. The v0.1.0 tag
remains unchanged.
