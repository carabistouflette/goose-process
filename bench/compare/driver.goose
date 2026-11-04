// SPDX-License-Identifier: Apache-2.0
import process from "../..";
import std;
import os;
fn main() {
    let argv = args();
    assert(argv.len == 4);
    let fixture: const u8[:] = argv[1];
    let mode:    const u8[:] = argv[2];
    let count, ok = parse_int(argv[3]);
    assert(ok && count > 0);
    var command: (const u8[:])[>..] = [];
    command.push(fixture); command.push(mode);
    if mode == "args" { for i in 128 { command.push("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"); } }
    var input:    u8[>..] = [];
    var expected: u8[>..] = [];
    var errors:   u8[>..] = [];
    if mode == "input" { input.resize(131072, 'x'); expected.resize(131072, 'x'); }
    if mode == "streams" || mode == "redirect" { expected.resize(65536, 'x'); }
    if mode == "streams" { errors.resize(65536, 'y'); }
    let environment = [process::EnvOverride { key: "PROBE", value: "goose" }];
    let options = process::Options {
        timeout_ms:        10000, output_limit: if mode == "input" { 131072 } else { 65536 },
        stderr:            if mode == "redirect" { process::Output.Null } else { process::Output.Capture },
        stdin:             if mode == "input" { process::Input.Bytes } else { process::Input.Null }, input: input[..],
        clear_environment: mode == "environment",
        environment:       if mode == "environment" { environment[..] } else { process::NO_ENVIRONMENT[..] },
    };
    var out: u8[>..] = [];
    var err: u8[>..] = [];
    for i in 3 {
        let status = process::run(command, options, out, err);
        assert(process::success(status) && out == expected && err == errors);
    }
    let started = clock_ns();
    for i in count {
        let status = process::run(command, options, out, err);
        assert(process::success(status) && out == expected && err == errors);
    }
    print((clock_ns() - started) / count);
}
