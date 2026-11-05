# Using goose-check on the library regressions

The optional developer workflow uses the public command checker to validate this
library's actual optimized/debug API and feature tests plus the command example.
It adds exact stdout/exit fixtures, binary argument/stdin cases, and explicit
mismatch, output-limit and launched-timeout rejection controls.

```sh
make setup
make checker-setup CC=gcc     # downloads/builds the exact CHECKER_REVISION
make checker-source-test checker-consumer CC=gcc
```

The compiler pin must match between projects. Setup places the checker under an
ignored content-addressed `.build/` directory, refuses changed/hidden-index or
mismatched sources, and never resets a sibling checkout. Run one serial Make invocation
per checkout (without `-j`). The subsequent consumer workflow is offline; it does not fetch
library dependencies or modify goose-check, Honk or the parent repository.
GCC CI runs this workflow as a developer-tool check. The library itself has no
runtime/build dependency on the checker; normal `make check test` is unchanged.
The checker bundles a reviewed older process release; the test executables link
this checkout's current library. This separation avoids a bootstrap dependency
on the version under test.

The explicit TOML fixture selects the common finite budget and stdout cap.
Arguments, executable and fixture paths remain explicit. Positive cases require
exact binary stdout and expected exits, including exit127. Negative controls
assert mismatch1 versus process-error125, check diagnostic classification, and
prove the timed sleeper reached its readiness marker. Normal stderr remains
inherited; expected-error controls capture it only to verify classifications.

The API tests intentionally leave a descendant alive when testing direct-child
capture timeout. Neither the library nor checker supervises that descendant.
The standalone Python controller is a Linux subreaper and cleans/reaps this test
fixture after the checker returns. It expects exactly one such fixture per API
suite and none for other cases. This is test-scope cleanup, not an added process
library or checker promise. On controller timeout, the private session is killed
before its leader is reaped; adopted identities are acquired as owned pidfds.
There are no external waiters. Kernel cleanup can exceed the command budget.

## Invocation-cost experiment

```sh
make checker-setup checker-consumer CC=gcc
# Commit measurement source first. Build/setup is outside timing.
taskset -c 0 make checker-overhead CC=gcc
```

Nine shuffled rounds retain ten checked calls in each fixture/route cell, plus
separate warmup records: empty exit, literal/binary argument framing and 64 KiB
binary stdin/stdout; direct controller, checker flags, and configured checker.
All raw timings, ordering, fixture lengths and hashes, source/compiler/binary/script identities,
CPU affinity and wall-clock context are retained. No memory inference is made.

The direct route caches fixture bytes in Python and uses communicate on fixed,
trusted, finite child outputs. It does not implement the checker's incremental
capture caps, strict file loading, signal guard or exceptional recovery policy.
Checker routes include their own startup, fixture reading, comparison and extra
process. Both routes also include the same controller ownership/cleanup checks.
The experiment quantifies this healthy-workload tool layer; it is not safety
contract parity or a general Python/Goose performance comparison. Shared-host
noise and all regressions must remain visible; optimize only after evidence.

### Retained local result, 2026-11-05

[Raw report](../bench/results/checker-overhead-2026-11-05.json), measured source
`ccbe654f84d4c80ca0063a3e256ab368b64d9250`, checker
`32de6c4783bf8d3242e042993dadd160a98b2945`. All 810 measured calls and nine
warmups passed. The controller retained four descriptors before/after and no
adopted children. Binary/compiler/driver identities and fixture contents were
checked unchanged after timing. The JSON retains fixture lengths/hashes; exact
bytes are reconstructible from the pinned driver.

For each cell, take the median of its ten calls in each round, then the median
of those nine round medians. Parentheses show the full range of round medians;
all individual calls remain in the report. Units are milliseconds per checked
invocation, including controller subprocess and cleanup overhead.

| Fixture | Direct controller | Checker flags | Configured checker |
| --- | ---: | ---: | ---: |
| Empty exit | 2.279 (2.172–2.520) | 3.760 (3.528–5.342) | 3.984 (3.764–4.142) |
| Literal arguments | 2.278 (2.203–2.520) | 3.792 (3.682–4.080) | 4.014 (3.925–12.788) |
| Binary 64 KiB | 2.703 (2.592–2.782) | 5.547 (4.990–6.757) | 5.752 (5.622–6.945) |

Pairing by round, the median checker-minus-direct differences were 1.481,
1.516 and 2.923 ms respectively. Configured-minus-flags differences were 0.226,
0.238 and 0.191 ms. These are local added tool costs under the unequal policies
above; they do not justify a library/runtime optimization or a speedup claim.
The argument/configured round reaching 12.788 ms is retained without filtering.

This short series ran 00:10:40–00:10:43 UTC on CPU0 of an Intel Core Ultra 7
258V, Linux 7.2.11/glibc 2.44, Python 3.14.9, GCC 16.2.1 and the repository-pinned Goose
compiler. Host load averages and exact versions are in the raw record. Affinity
does not provide isolation: this was a shared host, with no exclusive-core claim,
independent repeat, long-duration timing, memory evidence or generalization to
other hosts/workloads.
