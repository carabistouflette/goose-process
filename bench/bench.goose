// SPDX-License-Identifier: Apache-2.0
import process from "..";
import std;
import os;
fn main() {
    let argv = args();
    var iterations: i64 = 100;
    if argv.len > 1 {
        let parsed, ok = parse_int(argv[1]);
        if !ok || parsed < 1 || parsed > 100000 { exit(2); }
        iterations = parsed;
    }
    var out: u8[>..] = [];
    var err: u8[>..] = [];
    var options = process::Options { .. };
    // Modes separately measure capture allocation/pipe overhead and inheritance.
    for mode in 2 {
        options.capture = mode == 0;
        let warmup = process::run(["/bin/true"], options, out, err);
        if !process::success(warmup) { exit(1); }
        let start = clock_ns();
        for i in iterations {
            let status = process::run(["/bin/true"], options, out, err);
            if !process::success(status) { exit(1); }
        }
        let elapsed = clock_ns() - start;
        print(if options.capture { "capture" } else { "inherit" }, " ", elapsed / iterations);
    }
}
