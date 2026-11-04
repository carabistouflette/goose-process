# Shared native-kernel profiling

This diagnostic compares the Goose public wrapper with a Rust front end using
**the same C process engine**, not Rust std::process or an independent security
implementation. See [the method](README.md#shared-kernel-diagnostic-track).

Measured clean source `3a7e6f8f50101d4f8aac8386f58f4c5a4a1c86d3` on 2026-11-04.
[Raw samples and complete provenance](../results/linux-shared-kernel-2026-11-04.json).
Intel Core Ultra 7 258V, Linux 7.2.11-arch1-1, glibc 2.44, CPU0 affinity,
powersave governor, shared host without exclusive isolation/frequency control.
GCC 16.2.1; both native engines use -O2 -fPIC and the same source/settings.
Goose eadca40; Rust 1.101.0-nightly (822182db4), -C opt-level=2.

Nine shuffled rounds, six fixtures, two front ends: 108 successful samples,
three warmups and 40 timed checked calls each. Every exit and exact byte sequence
was verified. Numbers are microseconds per call: median [minimum–maximum] of
nine batch means, not individual-call latency percentiles.

| Fixture | Goose | Rust / shared engine |
| --- | ---: | ---: |
| empty | 581.3 [571.4–604.3] | 589.8 [574.9–638.0] |
| args | 649.3 [630.1–685.5] | 640.2 [628.0–688.7] |
| streams | 684.6 [669.2–710.1] | 680.8 [640.7–718.5] |
| input | 975.7 [967.8–1059.2] | 989.9 [974.5–1073.5] |
| environment | 536.0 [520.3–542.5] | 542.9 [530.1–605.4] |
| redirect | 645.4 [624.8–686.5] | 642.6 [619.5–668.7] |

All sample ranges overlap. The largest median difference is about 1.5% in this
session, with each front end lower on three fixtures. This does not establish
that one wrapper is faster. No reliable wrapper bottleneck was established, so
no safety checks were removed or speculative production optimization added.

Both front ends stage argv/input, zero-initialize reusable captures and call
gp_execute with identical timeout/cap/env/stdio settings. Shared code means
shared native defects; this is not independent evidence of security correctness.
The Rust fixture wrapper is not a supported library API.

A separate `strace -c` parent-only profile of empty/streams/input (three warmups
plus 10 checked calls) found 13 waits and 13 pidfd opens in each driver/fixture,
26 output pipe creations for each, and 104 input sends plus13 socket pairs for
the input fixture. Empty-child selected syscall counts were identical. Stream
read/poll counts varied with scheduling. Tracing included driver startup and
changes scheduling, so its elapsed costs are not timing results.
[Raw syscall summaries](../results/linux-shared-profile-2026-11-04.json) retain
the scope, strace version, source revision and executable hashes.

Reproduce the untraced run with:

```sh
python3 bench/compare/measure.py --shared-kernel --cpu 0 --rounds 9 --iterations 40
```

For the separate syscall diagnostic after building, run each compare-goose and
compare-shared driver with modes empty,streams,input and the same absolute
compare-fixture path:

```sh
strace -qq -c -e trace=read,poll,sendto,wait4,pipe2,pidfd_open,pidfd_send_signal,close,fcntl,socketpair \
  .build/compare-goose "$PWD/.build/compare-fixture" input 10
```

The report supports continuing with real-caller integration rather than changing
the public wrapper to chase differences smaller than the observed variability.
