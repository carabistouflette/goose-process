# goose-process

A small subprocess library for Goose. Apache-2.0.
Experimental release 0.1.0; the API may change before 1.0.
Requires Linux kernel 5.3+ and glibc 2.34+.

Runs argument arrays with bounded captures, per-child environment and cwd,
independent stdio routing, finite or incremental binary stdin, and cancellation deadlines.
Child handles support polling, signals and explicit cleanup. Absolute executables
are the default; PATH search is opt-in. No implicit shell. Native process operations
use a small C kernel; Goose owns validation, input and returned output.
Execution has a finite deadline by default. Supervised Child handles can explicitly
disable the overall deadline for long-lived services while retaining IO bounds.

[API contract](docs/API.md) · [Benchmarks](bench/README.md) · [Comparisons](bench/compare/README.md) ·
[Contributing](CONTRIBUTING.md) · [Security](SECURITY.md)

[Caller integration](docs/CALLERS.md) covers signals, lock retention and exceptional
child ownership. [Changelog](CHANGELOG.md) records release readiness.
[Validation evidence](docs/VALIDATION.md) separates library checks from caller tests.

## Quickstart

On Linux, install Git, CMake 3.27+, Make, a C/C++20 compiler and libc development
headers. Python 3.11+ is required for the checks and tests.

```sh
git clone https://github.com/carabistouflette/goose-process.git
cd goose-process
make setup
make check test
.toolchain/goose/build/goose run examples/command.goose
```

The example prints `hello from Goose`. Setup builds the exact compiler recorded
in [GOOSE_REVISION](GOOSE_REVISION), with optional graphics/audio disabled.
Clang and its sanitizer/libFuzzer runtimes are needed for the optional Clang
sanitizer and fuzz targets below.

## Usage

```goose
import process from "carabistouflette/goose-process@160d23bbac93146f1d0926563e33dd26b5aecfd8";
import std;

fn main() {
    var out: u8[>..] = [];
    var err: u8[>..] = [];
    let status = process::run(
        ["/usr/bin/printf", "%s", "hello from Goose"],
        process::Options { timeout_ms: 5000 }, out, err);
    if process::success(status) { print(out); }
    else { print("command failed: ", status.error, ", OS error ", status.os_error); }
}
```

Save the example as `main.goose` and run it with the pinned compiler. Declare
the package in the application's root source. The example pins a reviewed
library commit; use a full commit hash for reproducible application imports.
Dynamic argument arrays should use `(const u8[:])[>..]`;
when mixing an owned string with literals, bind its view as `const u8[:]` so
array inference preserves read-only element types.

`run` replaces both caller-owned output buffers and returns a fixed-size status.
`Error.None` means the command completed and captures reached EOF; inspect the
exit code or call `success` to check whether the program succeeded. Launch errors
are distinct from child exits 126/127. Partial output survives timeout/overflow.

See [the API contract](docs/API.md) for limits, signals, per-child environment,
descriptor handling, and exceptional cancellation failure. This is a process
runner, not a sandbox or a process-tree supervisor. Finite stdin uses a one-way
socket byte stream; use FD routing for programs requiring a real pipe or live
output. Active child handles require an appropriate pumping operation or close;
there is no automatic cleanup.
Explicit `Output.Stream` with `stream_step` supports bounded caller-buffer live
processing, including mixed capture. See the [streaming contract](docs/API.md#bounded-live-output).
Explicit `Input.Stream` with `io_step` and `close_stdin` supports bounded duplex
input without a library payload queue. See [incremental input and the worker example](docs/INCREMENTAL-STDIN.md).
The [sustained streaming evidence](bench/STREAM-SOAK.md) records a resident
five-minute run, quiescent resource observations, profiles and their limits.
Pipelines, PTYs, credential switching and Windows are outside the current scope.
See [the Rust/Go/Python feature comparison](docs/COMPARISON.md).

## Development

```sh
make setup
make format check test
make sanitize SANITIZER_CC=gcc
make sanitize SANITIZER_CC=clang
make bench BENCH_ITERATIONS=100
make compare-check # optional; requires Rust and Go as well
make compare
make fuzz FUZZ_RUNS=10000
make memcheck # optional; requires Valgrind
```

Setup requires Git, CMake 3.27+, C/C++20 compilers and Make. Tests require Python
3.11+, GCC/Clang and Linux `/proc`. `GOOSE_REVISION` pins the same compiler as
goose-json; other revisions are not guaranteed. An existing matching compiler
can be supplied with `make GOOSE=/absolute/path/to/goose ...`.

Validation covers Goose in-process, native and debug execution; compile-time
resource ownership and capture-view lifetime rejection; binary and adversarial argument oracles;
simultaneous stream saturation, exact limits and cancellation; signal defaults;
closed standard descriptors; descriptor/zombie leaks; concurrent callers; and
injected syscall failures. GCC and Clang ASan/UBSan instrument the native kernel.
CI also enables glibc fortification for the sanitizer matrix; fault injection
covers both ordinary and fortified libc entry points.
Protected pages check exact input/output boundaries, resource-limit tests check
descriptor exhaustion, and signal storms exercise interrupted cancellation.
Traced-child stop notifications have real and deterministic injected regressions.
Independent classification tests cover exact native input limits while refusing
execution; cancellation tests also cover early output closure and new sessions.
The classification model freezes time to isolate validation from scheduling;
real deadlines and child cleanup remain covered by the integration tests.
The optional Clang/libFuzzer target mutates native validation/setup inputs under
ASan/UBSan and refuses all execution through an intercepted spawn call.
The [benchmark](bench/README.md) records local end-to-end measurements and raw
samples; there is no universal throughput claim.

[Apache-2.0](LICENSE). Copyright 2026 goose-process contributors.
See [third-party and tooling notices](THIRD_PARTY.md).

An optional [goose-check consumer workflow](docs/CHECKER-CONSUMER.md) verifies
this checkout's real public regressions with exact stdout/exit fixtures and
binary stdin, using the pinned external developer tool. `make checker-setup`
performs the explicit download; `make checker-consumer` is offline afterwards.
