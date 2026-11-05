# goose-process

Run commands from Goose without a shell. Capture binary output, send input,
set a child environment or working directory, and cancel a child on a deadline.
Use a Child for live input/output or signal forwarding, or a Pipeline to connect
two commands.

Requires **Linux kernel 5.3+ and glibc 2.34+**. Apache-2.0. The API is experimental.

## Quickstart

On Linux, install Git, CMake 3.27+, Make, a C/C++20 compiler and libc development
headers. Python 3.11+ is required for the checks and tests.

```sh
git clone https://github.com/carabistouflette/goose-process.git
cd goose-process
make setup
.toolchain/goose/build/goose run examples/command.goose
```

The example prints `hello from Goose`. Setup builds the exact compiler recorded
in [GOOSE_REVISION](GOOSE_REVISION), with optional graphics/audio disabled.
Clang and its sanitizer/libFuzzer runtimes are needed for the optional Clang
sanitizer and fuzz targets below.

## Use it in a program

```goose
import process from "carabistouflette/goose-process@7f759bb22e4afcbe65c78dee20861509c3310a52";
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

Save this as `main.goose` and run `.toolchain/goose/build/goose run main.goose`.
Keep the package import in your application's root source. Pin a full commit for
reproducible builds; the example pins the implementation described here.

`run` replaces `out` and `err`. `success` checks both the library result and exit
code zero. Nonzero command exits are different from launch failures. Captures
contain bytes; timeout or overflow may leave only a prefix.

## Choose an interface

| Need | Use | Example |
| --- | --- | --- |
| Run a command and collect output | `run` | [command.goose](examples/command.goose) |
| Read output as it arrives | `start` + `stream_step` | [compiler.goose](examples/compiler.goose) |
| Send input in chunks while reading output | `start` + `io_step`, then `close_stdin` | [worker.goose](examples/worker.goose) |
| Connect two commands | `run_pipeline` or an owned `Pipeline` | [pipeline.goose](examples/pipeline.goose) |

See the [API](docs/API.md), [incremental-input guide](docs/INCREMENTAL-STDIN.md),
[pipeline guide](docs/PIPELINES.md) and [caller cleanup rules](docs/CALLERS.md).

## Defaults and limits

- Executables must be absolute paths. PATH lookup requires `search_path: true`
  and uses the child's environment. Arguments are passed literally.
- Default timeout: 30 seconds. Default capture: 64 KiB per stream. Each capture
  and finite input can be at most 1 MiB; argv can contain at most 4,096 entries.
- A supervised `Child` can explicitly use `timeout_ms: 0`. `run` and pipelines
  require positive deadlines. A deadline requests cancellation; kernel work and
  reaping can take longer.
- Finish every active owner by pumping to completion or calling close. There is
  no automatic cleanup. An exceptional `unreaped_pid` transfers cleanup to you.
- Only direct children are supervised. This is not a sandbox or process-tree
  supervisor. PTYs, job control, credentials and other OSes are unsupported.

## Main versus release

These docs describe main's implementation at `7f759bb22e4afcbe65c78dee20861509c3310a52`.
The experimental `v0.1.0` tag stays at `470c23f8244d17ff2597bca3da35f3a98cc3ca50`.
It has command execution and Child control, but **not** `Output.Stream`,
`Input.Stream` or managed pipelines. Read the docs at the revision you import.
No stable API or maintenance guarantee is implied by the experimental tag.

## Development and results

Run `make check test` for this checkout. See [CONTRIBUTING.md](CONTRIBUTING.md)
for sanitizer and fuzz commands, [validation records](docs/VALIDATION.md) for
coverage, and [benchmarks](bench/README.md) for measurements and source versions.
Historical results are not current-main speed claims.

[Security reporting](SECURITY.md) · [Apache-2.0](LICENSE) · [Notices](THIRD_PARTY.md)
