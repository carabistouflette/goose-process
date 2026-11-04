# goose-process

A small synchronous subprocess library for Goose. Apache-2.0.
Experimental API; no release yet. Requires Linux kernel 5.3+ and glibc 2.34+.

Runs explicit absolute executable paths with an argument array, optional working
directory, separate bounded stdout/stderr captures, and a cancellation deadline.
No implicit shell or PATH search. Native process operations use a small C kernel;
Goose owns validation and returned output. See `CONTRIBUTING.md` for development
and commit standards.

```goose
import process from "../goose-process";
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

Declare the package in the application's root source. This checkout has no
published remote yet. Dynamic argument arrays should use `(const u8[:])[>..]`;
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
Protected pages check exact input/output boundaries, resource-limit tests check
descriptor exhaustion, and signal storms exercise interrupted cancellation.
Traced-child stop notifications have real and deterministic injected regressions.
The optional Clang/libFuzzer target mutates native validation/setup inputs under
ASan/UBSan and refuses all execution through an intercepted spawn call.
The [benchmark](bench/README.md) records local end-to-end measurements and raw
samples; there is no universal throughput claim.

[Apache-2.0](LICENSE). Copyright 2026 goose-process contributors.
