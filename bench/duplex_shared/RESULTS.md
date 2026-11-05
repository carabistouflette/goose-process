# Shared-engine duplex cost, 2026-11-05

**Historical study.** Results and unchanged-source statements below apply to
the named revisions, not current main. Reproduce with those source, compiler and
dependency versions. See [current benchmark scope](../CURRENTNESS.md).

The shared-engine routes had close session medians and overlapping ranges.
Removing one duplicate native-state reload reduces generated work, but the
paired wall times do **not** establish a reliable session-latency improvement.
All native validation, deadline, pidfd and cleanup policies remain enabled.

## Same engine, different wrappers

The [method](README.md) links the public Goose driver and a fixed Rust FFI driver
to byte-identical Goose-built native objects. Both run 128 sequential framed
requests through the same C worker, with 31/4096-byte payloads, 257-byte stdout and 3-byte stderr
views, a 30-second native budget, and explicit EOF/drain/reap. This is not Rust
std::process or independent evidence that the shared engine is correct. Rust's
fixed configuration/owned arrays and Goose's general wrapper/resource model
still differ; complete public-API parity is not asserted.

Three clean-source series retain 180 measured sessions and 36 warmups each:

| Native version | Source | Complete record |
| --- | --- | --- |
| Baseline reload | `73c953ee754447f54ad49ee3606c5adecfbd8430` | [baseline](../results/wrapper-shared-baseline-2026-11-05.json) |
| Separate output argument | `5836b3e6d94dc659ab31d54aa0e05d6c56a64581` | [first candidate](../results/wrapper-shared-output-arg-2026-11-05.json) |
| Grouped private result | `e6f193ea5c1ae2cd927dfd2a82cff7e5ff66bddc` | [final candidate](../results/wrapper-shared-grouped-2026-11-05.json) |

Baseline Goose/Rust medians were 2.581/2.553 ms for 31-byte payloads and
4.848/4.758 ms for 4096-byte payloads. These observations cannot attribute the
prior Rust-standard-library difference to Goose wrapper work alone. Native
object hashes match within every phase; source/compiler/object/driver identities
are checked before and after. The exact-object claim follows the pinned
compiler's build plan and before/after hashes; it assumes no cache mutation
between link and copy. The compiler source selecting these paths was reviewed.

## Interleaved before/after sessions

The [final paired series](../results/wrapper-paired-grouped-2026-11-05.json)
uses frozen baseline and grouped-result executables, verifies source hashes
against immutable Git, and passes one byte-identical worker to both phases.
It retains 360 measured sessions and 72 separate warmups. Nine rounds shuffle
all eight phase/wrapper/payload cells. The table reports the median of nine
round medians, each over five sessions, and the full range of round medians.
Every invocation passed its byte/status oracle without an adopted child;
controller descriptors stayed four.

| Wrapper/payload | Before median ms [range] | Grouped median ms [range] |
| --- | ---: | ---: |
| Goose /31 | 2.359 [2.316–2.602] | 2.487 [2.326–2.572] |
| Rust FFI /31 | 2.497 [2.379–2.584] | 2.407 [2.321–2.566] |
| Goose /4096 | 4.788 [4.456–4.889] | 4.518 [4.411–4.878] |
| Rust FFI /4096 | 4.560 [4.418–4.738] | 4.464 [4.404–4.771] |

Ranges overlap and directions differ. These mixed results do not establish a
general speedup. Driver wall includes staging, launch, exchanges, byte checks,
EOF and ordinary cleanup; startup and frame construction are excluded.
Auxiliary CPU/outer wall include process startup and worker execution.

The argument-based candidate has two complete, separately retained paired
series, each with 360 measured sessions and 72 warmups: [first](../results/wrapper-paired-output-arg-first-2026-11-05.json)
and [repeat after recorder repair](../results/wrapper-paired-output-arg-repeat-2026-11-05.json).
They do not establish a reliable latency improvement either. The recorder's
startup validation previously preceded failure-report construction. The fixed
controller retains an [intentional missing-record rejection](../results/wrapper-startup-rejection-2026-11-05.json):
zero samples and failed status, distinct from a failed timed session. Normal
tests also reject corrupt frozen artifacts before launching any sample.

## Generated work and profiles

Baseline GCC -O2 assembly copies the full 192-byte child state after wait_impl
solely to report input openness. A private result now groups accepted bytes and
openness from the already validated local state. InvalidInput/InvalidState
guards, span/alias checks, metadata/transfer handling and legacy argument count
remain intact. Public/native signatures and owner layout do not change.
The native package fingerprint is refreshed; the compiler pin is unchanged.

Separate [baseline](../results/wrapper-profile-baseline-2026-11-05.json),
[argument candidate](../results/wrapper-profile-output-arg-2026-11-05.json) and
[grouped candidate](../results/wrapper-profile-grouped-2026-11-05.json)
Callgrind profiles match the corresponding frozen driver/worker hashes. Each
record has one successful large-payload run per wrapper. They include startup,
runtime and byte checks, exclude worker executable instructions, and are not
untraced timing samples or isolated library instruction counts.

| Whole profiled driver | Baseline instructions | Separate argument | Grouped result |
| --- | ---: | ---: | ---: |
| Goose |3,030,772|3,019,712|2,987,439|
| Rust FFI |2,724,020|2,713,908|2,680,653|

The first candidate removed the copy but added helper work. Grouping outputs
preserves the original helper argument count. Final whole-driver instruction
counts are about 1.43%/1.59% lower in these profiles. This does not establish a
general latency or CPU-speed guarantee. The source/ABI proof and modest
reduction in actual generated work justify this small internal change without
removing safety checks.

## Other validation

The unequal-policy four-adapter comparison was rerun for both candidates,
with 360 measured sessions and 72 warmups each: [argument](../results/wrapper-stdlib-output-arg-2026-11-05.json)
and [grouped](../results/wrapper-stdlib-grouped-2026-11-05.json). All passed.
Final grouped medians for 31/4096 payloads were Goose 2.378/4.505 ms, Rust std
2.082/4.194 ms, Go 3.768/4.231 ms and Python 5.352/10.767 ms. The FFI and Rust
standard-library tracks remain separate; no language-wide ranking follows.

The [final resident record](../results/wrapper-soak-grouped-2026-11-05.json)
contains 2,562 measured cases plus 12 warmups over 300.396593840 seconds,
427 of each declared duplex case. All passed. Every measured quiescent
checkpoint had three descriptors, no direct child, RSS 2,712 KiB, 79 mappings and
VmSize 33,575,152 KiB. Controller descriptors stayed four, with no adopted fixture.
This is not a peak, child-memory, allocator-accounting or cross-run RSS
improvement measurement.

That soak ran from a clean local clone of final native source e6f193e, with
spaces in its path, a fresh Goose cache and the matching existing pinned
compiler. It is not a cold compiler bootstrap or anonymous-public-clone
execution claim. CPU0 affinity was inherited; final normal/sanitizer validation
on CPU2 overlapped part of the soak. All timing series used shared CPU0;
external load, SMT, frequency, temperature and caches were uncontrolled.
GCC -O2/-fPIC native objects, actual tool versions, flags, platform, affinity,
hashes, timestamps, all samples and normalized profile logs are retained.
Original logs are hashed before checkout-prefix normalization.

Final normal GCC/Clang suites, both ASan/UBSan compilers, intercepted-spawn
10,000-run fuzzing, successful/rejected shared-worker checks and Clang static
analysis of both native sources passed. An earlier analysis invocation used
one output file for two inputs; analyzing each separately corrected that
tooling error. Initial-candidate validation is not substituted for final checks.

Independent design, Rust FFI, Standards and Spec reviews found no native
correctness blocker. The Spec recorder-retention finding is fixed and tested.
A separate final grouped-result review confirmed initialization, alias/return
guards, pointer lifetime, accepted counts, transfer and legacy equivalence.
Final evidence review accepted all 13 reports, verified source/frozen-object/log
identities and recomputed the documented aggregation. Hosted exact-head CI is a
separate publication gate.

## Reproduction

Use [the shared-engine build method](README.md), preserving the build directory
and report for each committed source version, then interleave them:

```sh
taskset -c 0 python3 bench/duplex_pair.py --before .build/before.json --after .build/after.json --before-directory .build/before --after-directory .build/after
python3 bench/duplex_compare/profile.py --measurement .build/after.json --directory .build/after --tool callgrind
```

The paired runner never rebuilds its frozen inputs. Supplied phase reports
must be complete; their artifacts and source Git objects must verify. Keep
distinct directories and all records. No release tag, application pin or parent
catalog is changed by these reproduction commands.
