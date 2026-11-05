# Recorded standard-library comparison

**Historical study.** Results and unchanged-source statements below apply to
the named revisions, not current main. Reproduce with those source, compiler and
dependency versions. See [current benchmark scope](../CURRENTNESS.md).

Source: clean commit
[`12c509e49ec0b1a83c2621db4dea0d7f2001645d`](https://github.com/carabistouflette/goose-process/commit/12c509e49ec0b1a83c2621db4dea0d7f2001645d),
2026-11-04. [Raw samples and provenance](../results/linux-stdlib-2026-11-04.json).
Reproduce from that commit with `python3 bench/compare/measure.py --cpu 0
--rounds 9 --iterations 40`. See [method and limitations](README.md).

Intel Core Ultra 7 258V, Linux 7.2.11-arch1-1, x86-64, glibc 2.44;
CPU0 affinity, powersave governor, shared workstation without exclusive isolation
or frequency control. GCC 16.2.1 with `-O2`; Goose eadca40 (runtime 3025e5810379).
Rust **1.101.0-nightly** (822182db4, 2026-10-13), `-C opt-level=2`, stable process
APIs only; Go **go1.27.3-X:nodwarf5**, `-trimpath`; Python **3.14.9**.
These installed builds differ from the documentation versions in the historical
API research. The report retains their full version output.

Nine shuffled rounds, six fixtures, four drivers: **216 successful sample
processes**, each with three warmups and 40 checked timed calls. No failed sample
was omitted. Numbers below are **microseconds per call: median of nine batch
means [minimum–maximum batch mean]**, not individual-call latency percentiles.
Every child exit and exact output byte sequence was checked inside timing.

| Fixture | Goose | Rust | Go | Python |
| --- | ---: | ---: | ---: | ---: |
| empty | 603.8 [583.7–668.0] | 588.7 [564.8–647.8] | 775.6 [707.6–842.6] | 824.8 [803.4–878.7] |
| args | 667.2 [649.0–719.4] | 622.1 [600.2–756.9] | 803.1 [780.8–843.0] | 886.0 [850.5–959.0] |
| streams | 702.8 [665.7–764.5] | 732.7 [725.6–792.6] | 1067.0 [996.9–1128.4] | 1054.5 [1016.0–1123.1] |
| input | 1054.4 [997.4–1077.1] | 1008.6 [975.8–1103.2] | 1365.1 [1330.0–1485.3] | 1536.9 [1483.2–1678.1] |
| environment | 581.7 [559.5–646.9] | 536.9 [512.5–568.8] | 682.8 [661.6–748.5] | 804.3 [763.1–946.6] |
| redirect | 669.0 [641.8–721.9] | 631.2 [617.5–682.8] | 878.7 [851.9–946.7] | 863.6 [829.0–915.6] |

Goose's median batch cost was lower than Go's and Python's on all six fixtures
in this session. Rust's was lower on five; Goose's was lower on two-stream
capture. Several sample ranges overlap. These measurements do **not** establish
universal speed superiority, a statistical ranking, or equal security costs.

All drivers execute the same successful argv, environment and binary I/O work.
Goose keeps hard capture caps, a finite deadline, reset-all child signal policy
and close-from FD isolation. Standard-library baseline captures are uncapped
without a per-child timeout; Python already closes foreign FDs and restores
selected signals, while Rust/Go policies differ. Input is a socket for Goose and
pipes for the baselines; Rust additionally starts/joins a writer thread for each
input call. Capture allocation/reuse strategies differ. The complete differences
and scope are in [the harness documentation](README.md).

This session measured successful workloads with the bounded defaults above. It does not justify removing safety checks or adding
complexity to chase differences smaller than host variability. For a stronger
claim, independently validate matched safety-contract adapters, collect repeated
sessions on the deployment hardware, and measure failure/overflow cleanup as a
separate behavior suite.
