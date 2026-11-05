# Checked duplex sessions, 2026-11-05

All **360 measured sessions and 72 warmups** passed the exact frame, output,
EOF and exit-status checks. This is a shared-host successful-workload comparison
of four bounded adapters, with the [documented policy differences](README.md).
It does not establish equal safety contracts or a general speed ranking.

## Final byte-span oracle series

[All raw results](../results/duplex-compare-2026-11-05.json) come from clean
`e52219364fab917e901e7a578e34c53d6880139a`. Source/compiler/driver/worker
identities were checked before and after; controller descriptors stayed at four and
no adopted child remained. Each session starts one worker, performs 128 sequential
requests, verifies every byte, explicitly closes stdin, drains and reaps. Frame
construction, driver startup/imports and compilation are outside the driver timer.
Adapter setup, byte checks and ordinary cleanup are inside it.

Each cell has nine round medians, each over five measured sessions; one separate
warmup precedes every cell. All eight cells are shuffled each round with seed
20261105. Each invocation starts a new driver process: a warmup does not keep
the next invocation's language runtime resident.

| Adapter | 31-byte payload: median ms [round range] | 4096-byte payload: median ms [round range] |
| --- | ---: | ---: |
| Goose | 2.461 [2.379–2.705] | 4.853 [4.498–5.086] |
| Rust | 2.112 [2.071–2.275] | 4.429 [4.216–4.744] |
| Go | 3.758 [3.239–3.977] | 4.389 [3.989–5.652] |
| Python | 5.639 [5.276–6.702] | 11.240 [10.710–11.814] |

The 31-byte case checks 4,480 echoed bytes and 512 stderr bytes; the 4096-byte case
checks 524,800 echoed bytes and 512 stderr bytes. These are whole 128-request
session times, not individual-request latency samples. Rust had lower medians
than Goose here. The larger-payload Goose/Rust/Go ranges overlap. Neither these
results nor one shared-host series warrants a universal performance claim.

The actual tools were Goose `eadca40` (runtime `3025e5810379`, full compiler pin
in JSON), GCC 16.2.1, Rust 1.101.0-nightly (`822182db4`), Go 1.27.3-X:nodwarf5
and Python 3.14.9. Worker C uses `-O2`; Goose uses `--cc gcc -O2`, Rust uses
`-C opt-level=3 -D warnings --edition2021`, and Go uses `build -trimpath` with
local/offline toolchain settings. CPU0 affinity is inherited by drivers and
children on an Intel Core Ultra 7 258V, Linux 7.2.11/glibc 2.44. This is a shared
host without exclusive cores, controlled caches, temperature/frequency or SMT
isolation. No other heavy local job was launched during this short series;
external host activity was not controlled. Auxiliary outer wall and process-tree
CPU include startup/imports and worker CPU; they are not isolated library CPU.

## Retained first series and profiling

The [first complete series](../results/duplex-compare-before-span-2026-11-05.json)
at `25e9b37fa15b65054dd9136e5a01f84d4e4b2f05` also passed all 360 measured
sessions and 72 warmups. It used per-byte checked loops in Goose/Go while Rust/
Python compared spans. Its Goose medians were 2.702/5.290 ms; all samples are
retained, rather than replaced by the later implementation's results.

[Separate profiles](../results/duplex-profile-before-span-2026-11-05.json) at
`45e311c58d8b1a586c12dd7526040e2d107fb50f` used the exact first-series
Goose/Rust executables and worker. Ten successful profiles covered parent-only
strace and Memcheck at both payloads, plus Callgrind at 4096 bytes. Every
instrumented run checked final byte totals and exit status. Instrumented wall
times are not included in either untraced series.

| Selected parent syscalls | Goose31 | Rust31 | Goose4096 | Rust4096 |
| --- | ---: | ---: | ---: | ---: |
| poll |390|390|2,179|2,179|
| read |393|393|2,312|2,312|
| wait4 |1|3|1|2|
| All selected calls |925|927|4,633|4,634|

Strace excludes child executable work and counts only the listed selected
syscalls. Similar counts do not establish identical kernel costs. Callgrind
recorded 8,013,812 whole-driver instructions for the original Goose large case;
68.70% were attributed to generated `main_g.constprop.0`, which combines
inlined code and benchmark checks. This did not isolate a runtime defect.

The justified change was in the benchmark: compare complete returned spans with
the same byte-equality oracle in Goose and Go. Request permissions, read bounds,
all bytes, cleanup and deadlines remain checked. The [follow-up Goose profile](../results/duplex-profile-span-2026-11-05.json)
at `1395b4f8a3c0e6895e27dee747b2bfd387cc77f9` matches the final-series
binary and records 3,030,785 whole-driver instructions, with 10.08% in that
generated main symbol. The native wait_impl count stayed approximately 1.125M.
This is evidence of less benchmark-checking work, not a library runtime speedup.
The two untraced series remain separate shared-host observations.

Baseline Memcheck reported zero errors and zero definitely/indirectly/possibly
lost bytes in all four runs. Reachable runtime heap was 66,056 bytes for Goose
and 548 for Rust. These tool summaries are not comparable whole-process memory
use, and do not fully account for Goose's mmap-backed storage. The final-span
follow-up is Callgrind only, not a new Memcheck run. Cancellation/SIGPIPE/fault
paths are outside these successful-session profiles; no profiler workaround or
weakened library safety policy was introduced. Logs normalize only the checkout
prefix and retain raw-log hashes; some debug-source annotation files were absent.

## Reproduce and validate

```sh
make setup
python3 bench/duplex_compare/check.py
taskset -c 0 python3 bench/duplex_compare/measure.py --rounds 9 --calls 5 --output .build/current-duplex.json
taskset -c 0 python3 bench/duplex_compare/profile.py --measurement .build/current-duplex.json --output .build/current-profile.json
taskset -c 2 python3 bench/duplex_compare/check.py --deadlines
```

Commit source before timing/profiling. Profiling requires the measured binaries
to remain unchanged. The optional selectors `--language goose --tool callgrind`
run only the large Goose instruction profile. Tool versions/hashes, raw logs,
source revisions and actual flags are retained. Compiler/worker hashes do not
freeze every shared library, host configuration or language runtime dependency.

Independent Standards review passed. Spec review found three adapter/provenance
issues before measurement: Go prefetched future frames, its timer omitted channel
setup, and watchdog exceptions lost partial diagnostics. All were fixed; tests
now reject sixteen actual bad-worker cases across the four languages, and prove
watchdog diagnostic retention. Eight final actual 30-second deadline checks
pass separately with no adopted child. Independent evidence review recomputed
the resident soak and first series and matched their source/binary/tool hashes.
See [resident validation and its limits](../../docs/DUPLEX-VALIDATION.md).
