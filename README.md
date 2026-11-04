# goose-process

A small synchronous subprocess library for Goose. Apache-2.0.
Experimental API; no release yet. Requires Linux kernel 5.3+ and glibc 2.34+.

Runs explicit absolute executable paths with an argument array, optional working
directory, separate bounded stdout/stderr captures, and a cancellation deadline.
No implicit shell or PATH search. Native process operations use a small C kernel;
Goose owns validation and returned output.

[API contract](docs/API.md) · [Benchmarks](bench/README.md) ·
[Contributing](CONTRIBUTING.md) · [Security](SECURITY.md)

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
import process from "carabistouflette/goose-process@9fd737c08f491ff842725c16189073891c8cfff7";
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

See [the API contract](docs/API.md) for limits, signals, inherited environment,
descriptor handling, and exceptional cancellation failure. This is a process
runner, not a sandbox or a process-tree supervisor. The first version excludes
PATH lookup, environment overrides, stdin streaming, pipelines and Windows.

## Development

```sh
make setup
make format check test
make sanitize SANITIZER_CC=gcc
make sanitize SANITIZER_CC=clang
make bench BENCH_ITERATIONS=100
make fuzz FUZZ_RUNS=10000
make memcheck # optional; requires Valgrind
```

Setup requires Git, CMake 3.27+, C/C++20 compilers and Make. Tests require Python
3.11+, GCC/Clang and Linux `/proc`. `GOOSE_REVISION` pins the same compiler as
goose-json; other revisions are not guaranteed. An existing matching compiler
can be supplied with `make GOOSE=/absolute/path/to/goose ...`.

Validation covers Goose in-process, native and debug execution; compile-time
input/output ownership rejection; binary and adversarial argument oracles;
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
