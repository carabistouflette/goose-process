// SPDX-License-Identifier: Apache-2.0
import process from "..";
import std;
import os;
fn measure(name: const u8[:], command: const (const u8[:])[:], options: process::Options, iterations: i64) {
    var out: u8[>..] = [];
    var err: u8[>..] = [];
    let warmup = process::run(command, options, out, err);
    if !process::success(warmup) { exit(1); }
    let start = clock_ns();
    for i in iterations {
        let status = process::run(command, options, out, err);
        if !process::success(status) { exit(1); }
    }
    print(name, " ", (clock_ns() - start) / iterations);
}
fn main() {
    let argv = args();
    var iterations: i64 = 100;
    if argv.len > 1 {
        let parsed, ok = parse_int(argv[1]);
        if !ok || parsed < 1 || parsed > 100000 { exit(2); }
        iterations = parsed;
    }
    let helper_text = str(resource_dir(), "helper");
    let helper: const u8[:] = helper_text[..];
    measure("capture", ["/bin/true"], process::Options { .. }, iterations);
    measure("inherit", ["/bin/true"], process::Options { capture: false }, iterations);
    measure("maxcap-empty", ["/bin/true"], process::Options { output_limit: 1048576 }, iterations);
    measure("streams-64k", [helper, "streams", "65536"], process::Options { .. }, iterations);
    measure("streams-1m", [helper, "streams", "1048576"], process::Options { output_limit: 1048576 }, iterations);
    var argument: u8[>..] = [];
    argument.resize(16384, 'x');
    let view: const u8[:] = argument[..];
    measure("large-argv", ["/bin/true", view, view, view], process::Options { .. }, iterations);
}
