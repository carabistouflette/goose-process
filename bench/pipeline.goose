// SPDX-License-Identifier: Apache-2.0
import process from "..";
import std;
import os;

fn completed(status: process::PipelineStatus) -> bool {
    return process::pipeline_success(status) && status.os_error == 0 &&
        status.first.os_error == 0 && status.second.os_error == 0 &&
        status.first.signal == 0 && status.second.signal == 0 &&
        status.first.unreaped_pid == 0 && status.second.unreaped_pid == 0;
}
fn captures(out: const u8[:], first_err: const u8[:], second_err: const u8[:],
    input: const u8[:], prefix: i64, errors: i64) -> bool {
    if out.len != prefix + input.len || first_err.len != errors || second_err.len != errors { return false; }
    for i in out.len {
        let expected: u8 = if i < prefix { 'x' } else { input[i - prefix] };
        if out[i] != expected { return false; }
    }
    for byte in first_err { if byte != 'y' { return false; } }
    for byte in second_err { if byte != 'z' { return false; } }
    return true;
}
fn copied(first: const (const u8[:])[:], first_options: process::Options,
    second: const (const u8[:])[:], second_options: process::Options,
    input: const u8[:], prefix: i64, errors: i64) {
    // All caller buffers and the convenience operation's fresh owner are timed.
    var out: u8[>..] = []; var first_err: u8[>..] = []; var second_err: u8[>..] = [];
    let result = process::run_pipeline(first, first_options, second, second_options, out, first_err, second_err);
    assert(completed(result));
    assert(captures(out[..], first_err[..], second_err[..], input, prefix, errors));
    // A successful run_pipeline has waited for both children and all capture EOFs.
}
fn borrowed(first: const (const u8[:])[:], first_options: process::Options,
    second: const (const u8[:])[:], second_options: process::Options,
    input: const u8[:], prefix: i64, errors: i64) {
    var owner = process::Pipeline { .. };
    let began = process::start_pipeline(first, first_options, second, second_options, owner);
    if began.error != process::Error.None || !began.first_launched || !began.second_launched {
        let closed = process::close_pipeline(owner);
        assert(closed.first.unreaped_pid == 0 && closed.second.unreaped_pid == 0);
        assert(false);
    }
    let result = process::wait_pipeline(owner);
    let terminal, done = process::pipeline_step(owner);
    // Do not let an assertion bypass explicit owner cleanup.
    let valid = completed(result) && done && completed(terminal) &&
        captures(process::pipeline_stdout(owner), process::pipeline_first_stderr(owner),
            process::pipeline_second_stderr(owner), input, prefix, errors);
    let closed = process::close_pipeline(owner);
    assert(valid && completed(closed));
}
fn main() {
    let argv = args(); assert(argv.len == 3);
    let convenience = argv[1] == "convenience"; assert(convenience || argv[1] == "owner");
    let pressure = argv[2] == "pressure"; assert(pressure || argv[2] == "tiny");
    let helper_text = str(resource_dir(), "pipeline-helper"); let helper: const u8[:] = helper_text[..];
    var input: u8[>..] = [];
    if pressure {
        input.resize(262144, 0); for i in input.len { input[i] = (i % 256) as u8; }
    } else { input.append("\x00\xff\x01"); }
    let prefix: i64         = if pressure { 65536 } else { 0 };
    let errors: i64         = if pressure { 65536 } else { 0 };
    let amount: const u8[:] = if pressure { "65536" } else { "0" };
    let first = [helper, "source", amount, amount]; let second = [helper, "copy", amount];
    let first_options = process::Options {
        timeout_ms: 30000, stdin: process::Input.Bytes, input: input[..], stderr_limit: 65536,
    };
    let second_options = process::Options {
        timeout_ms: 30000, stdout_limit: prefix + input.len, stderr_limit: 65536,
    };
    let began = clock_ns();
    if convenience { copied(first[..], first_options, second[..], second_options, input[..], prefix, errors); }
    else { borrowed(first[..], first_options, second[..], second_options, input[..], prefix, errors); }
    let elapsed = clock_ns() - began;
    // The route has returned: fresh owner/buffer destruction and cleanup are complete.
    print(elapsed, " ", prefix + input.len, " ", errors, " ", errors);
}
