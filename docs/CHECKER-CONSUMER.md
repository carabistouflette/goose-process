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
mismatched sources, and never resets a sibling checkout. Run one Make invocation
per checkout. The subsequent consumer workflow is offline; it does not fetch
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
All raw timings, ordering, byte oracles, source/compiler/binary/script identities,
CPU affinity and wall-clock context are retained. No memory inference is made.

The direct route caches fixture bytes in Python and uses communicate on fixed,
trusted, finite child outputs. It does not implement the checker's incremental
capture caps, strict file loading, signal guard or exceptional recovery policy.
Checker routes include their own startup, fixture reading, comparison and extra
process. Both routes also include the same controller ownership/cleanup checks.
The experiment quantifies this healthy-workload tool layer; it is not safety
contract parity or a general Python/Goose performance comparison. Shared-host
noise and all regressions must remain visible; optimize only after evidence.
