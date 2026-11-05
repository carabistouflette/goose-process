# Shared-native-engine duplex diagnostic

**Historical study.** Results and unchanged-source statements below apply to
the named revisions, not current main. Reproduce with those source, compiler and
dependency versions. See [current benchmark scope](../CURRENTNESS.md).

This track compares the public Goose wrapper with a fixed Rust FFI wrapper that
links **the exact native object bytes used by Goose**. It separates wrapper,
staging and checking costs from the different process policies in the
[standard-library adapter comparison](../duplex_compare/README.md). It is not
`std::process`, an independent native implementation, full public-API parity or
independent security evidence: a native engine bug is shared by both routes.

[Recorded versions, paired sessions, profiles and limits](RESULTS.md) retain all
three native variants and the final validation results.

From a standalone Linux checkout, with the pinned Goose compiler, GCC, installed
Rust, `ar` and Python 3.11+:

```sh
make setup
python3 bench/duplex_shared/measure.py --build-only
python3 bench/duplex_shared/check.py
# Commit source first. CPU affinity is inherited, not exclusive isolation.
taskset -c 0 python3 bench/duplex_shared/measure.py --rounds 9 --calls 5
```

`--directory .build/another-shared-build` selects another build directory,
allowing reviewed callers to preserve a prior executable/object set. The
scheduler itself builds one current source revision, not a before/after pair.
The report defaults to `.build/duplex-shared.json`; use `--output PATH` to retain
it elsewhere. Build-only checks permit development edits and do not record a
timing series. Ordinary measurement requires a clean checkout and verifies Git,
source, compiler, object and executable identities before/after.

## Exact engine and fixed workload

The scheduler builds the unchanged public Goose
`bench/duplex_compare/driver.goose`, then parses the pinned compiler's quoted
`--dry-run -v` plan to identify the two native object files. It reads paths only;
it never executes emitted shell text. Byte-identical copies of those objects
form the static archive linked by Rust. The report records both original and
copied object hashes and rejects any changed identity. Native source fingerprints
must be current. GCC native compilation is `-O2 -fPIC -std=c11 -Wall -Wextra
-Werror` through the existing package build; no safety policy is disabled.

Both drivers launch the same absolute executable compiled unmodified from
`examples/worker.c`. A session performs 128 sequential request/response exchanges
using one child. Payload is 31 or 4096 bytes, framed by a four-byte big-endian
length; payload byte is `(index*17 + index/251) % 256`. The fixed frame is built
before the timer and reused. Both wait for an exact echo and `ack\0` on stderr
before advancing the request. Reads use 257-byte stdout and 3-byte stderr views;
all bytes and counts, early/excess output, final EOF and exit0 are checked.
Totals are 4,480 or 524,800 stdout/input bytes and 512 stderr bytes per session.

The native configuration is identical:
`[30000,6,6,4,-1,-1,-1,0,0,65536,65536]`, representing a 30-second deadline,
independent Stream outputs and incremental Stream input. The same engine keeps
its socket stdin, pidfd ownership, signal reset, descriptor isolation, argument
validation, per-call bounds and alias checks, accepted-count/EOF handling and
direct-child cleanup. Stream output is caller-buffered rather than a growing
capture; fixed response oracles enforce total expected bytes. Both close stdin,
drain outputs, reap, then close the owner.

The Rust interface is deliberately private and fixed: 24 i64 state words,
six metadata words and three progress words, matching `native/process.h` at the
recorded revision. An incompatible ABI must update this fixture; it is not a
supported Rust binding. Its exclusive owner closes on ordinary oracle errors.
Any unexpected transferred PID is explicitly diagnosed and fails the fixture;
recovery under denied native cleanup remains the standalone controller's test
boundary, not a production transferred-ownership policy implemented here.

`check.py` runs all four successful driver/payload combinations and eight
rejections using actual early-EOF, corrupt, excess-after-EOF and frontloaded
worker variants. Private sessions and the existing standalone subreaper
controller ensure ordinary failures leave no adopted child. The controller
signals its exclusively owned, unreaped private group before reaping the leader
on watchdog failure; it never reaps a living driver's native-owned child.

## Timing and interpretation

Both timers begin before owner availability/path validation/copied argv staging,
then include launch, all exchanges, byte equality oracles, explicit EOF,
drain/reap and normal close. Driver startup/imports, argument parsing, fixed
frame construction and compilation are excluded. The configuration and empty
environment/cwd are fixed; Rust mirrors relevant executable and per-call span
bounds but does not reproduce Goose's general Options model, dynamic route
checks, growable captures or compiler-enforced resource model. Rust's owned
arrays and Goose's wrapper/runtime still have different validation/allocation
and machine-code costs. Shared engine policy does not make every public wrapper
operation or memory safety mechanism identical.

Each of nine default rounds shuffles four wrapper/payload cells with seed
20261106. A cell retains one separate warmup and five measured sessions. Every
session starts a new driver process; warmup does not keep a language runtime
resident. Summaries should use the median of round medians and retain ranges.
All sample outputs/statuses/order and failure context are retained, including
watchdog partial stdout/stderr observed before cleanup via the existing recorder.
Auxiliary outer wall and process-tree CPU include startup/imports and worker CPU;
they are not isolated wrapper CPU. No resident-memory measurement is made here.

Only the fixed successful protocol is compared. The host is shared; CPU affinity
does not control other processes, SMT, caches, frequency or temperature. The
result cannot establish a general language speed/safety/memory ranking, prove
absence of bugs, or justify removing validation, deadline or cleanup checks.
Profile independently before changing code and retain matched before/after
identities when evaluating an optimization.
