// SPDX-License-Identifier: Apache-2.0
import process from "..";
import std;
import os;

fn clean(status: process::PipelineStatus) {
    assert(status.first.unreaped_pid == 0 && status.second.unreaped_pid == 0);
}
fn checked(owner: process::Pipeline&, valid: bool) {
    if !valid { clean(process::close_pipeline(owner)); assert(false); }
}
fn reject(helper: const u8[:], marker: const u8[:], first: process::Options, second: process::Options) {
    var out: u8[>..] = [1]; var first_err: u8[>..] = [2]; var second_err: u8[>..] = [3];
    let result = process::run_pipeline([helper, "mark", marker], first,
        [helper, "mark", marker], second, out, first_err, second_err);
    clean(result);
    let launched = file_exists(marker);
    if launched { assert(delete_file(marker)); }
    assert(result.error == process::Error.InvalidInput && !result.first_launched && !result.second_launched && !launched);
    assert(out.len == 0 && first_err.len == 0 && second_err.len == 0);
}
fn main() {
    let helper_text = str(resource_dir(), "pipeline-helper"); let helper: const u8[:] = helper_text[..];
    let marker_text = str(resource_dir(), "pipeline-marker-", time_ns()); let marker: const u8[:] = marker_text[..];
    var owner = process::Pipeline { .. };
    var input: u8[>..] = []; for i in 524288 { input.push((i % 256) as u8); }
    let began = process::start_pipeline([helper, "source", "262144", "262144"], process::Options {
        stdin: process::Input.Bytes, input: input[..], stderr_limit: 262144,
    }, [helper, "copy", "262144"], process::Options {
        stdout_limit: 786432, stderr_limit: 262144,
    }, owner);
    checked(owner, began.error == process::Error.None && began.first_launched && began.second_launched);
    let pressure = process::wait_pipeline(owner); clean(pressure);
    assert(process::pipeline_success(pressure));
    assert(process::pipeline_stdout(owner).len == 786432);
    for i in 786432 {
        let expected: u8 = if i < 262144 { 'x' } else { ((i - 262144) % 256) as u8 };
        assert(process::pipeline_stdout(owner)[i] == expected);
    }
    assert(process::pipeline_first_stderr(owner).len == 262144 && process::pipeline_second_stderr(owner).len == 262144);
    for byte in process::pipeline_first_stderr(owner) { assert(byte == 'y'); }
    for byte in process::pipeline_second_stderr(owner) { assert(byte == 'z'); }
    let repeated, complete = process::pipeline_step(owner);
    clean(repeated); assert(complete && process::pipeline_success(repeated));
    assert(process::pipeline_success(process::close_pipeline(owner)));

    var out: u8[>..] = []; var first_err: u8[>..] = []; var second_err: u8[>..] = [];
    let arguments = process::run_pipeline([helper, "args", "a b", "$(not-run)", "*", ";"], process::Options { .. },
        [helper, "copy", "0"], process::Options { .. }, out, first_err, second_err);
    clean(arguments); assert(process::pipeline_success(arguments));
    assert(out == "a b\x00$(not-run)\x00*\x00;\x00" && first_err.len == 0 && second_err.len == 0);

    // Exercise each independent capture at an exact cap, one over, empty/zero,
    // and nonempty/zero. Intermediate pipeline bytes are not accumulated here.
    for channel in 3 {
        for boundary in 4 {
            let count: i64 = if boundary < 2 { 4096 } else if boundary == 2 { 0 } else { 1 };
            let limit: i64 = if boundary == 0 { 4096 } else if boundary == 1 { 4095 } else { 0 };
            let text = str(count); let n: const u8[:] = text[..];
            let source_bytes = if channel == 0 { n } else { "0" };
            let source_errors = if channel == 1 { n } else { "0" };
            let sink_errors = if channel == 2 { n } else { "0" };
            let result = process::run_pipeline([helper, "source", source_bytes, source_errors], process::Options {
                stderr_limit: if channel == 1 { limit } else { 65536 },
            }, [helper, "copy", sink_errors], process::Options {
                stdout_limit: if channel == 0 { limit } else { 65536 },
                stderr_limit: if channel == 2 { limit } else { 65536 },
            }, out, first_err, second_err);
            clean(result);
            if count <= limit { assert(process::pipeline_success(result)); }
            else { assert(result.error == process::Error.OutputLimit); }
            let target = if channel == 0 { out[..] } else if channel == 1 { first_err[..] } else { second_err[..] };
            assert(target.len == min(count, limit));
            let expected: u8 = if channel == 0 { 'x' } else if channel == 1 { 'y' } else { 'z' };
            for byte in target { assert(byte == expected); }
            if channel != 0 { assert(out.len == 0); }
            if channel != 1 { assert(first_err.len == 0); }
            if channel != 2 { assert(second_err.len == 0); }
        }
    }
    let exits = process::run_pipeline([helper, "exit", "7"], process::Options { .. },
        [helper, "exit", "9"], process::Options { .. }, out, first_err, second_err);
    clean(exits); assert(exits.error == process::Error.None && exits.first.exit_code == 7 && exits.second.exit_code == 9);
    assert(exits.first_launched && exits.second_launched && !process::pipeline_success(exits));
    let early = process::run_pipeline([helper, "flood"], process::Options { .. },
        [helper, "exit", "0"], process::Options { .. }, out, first_err, second_err);
    clean(early); assert(early.error == process::Error.None && early.first.signal == 13 && early.second.exit_code == 0);

    let missing = "/proc/self/goose-process-pipeline-missing";
    let first_failure = process::run_pipeline([missing], process::Options { .. },
        [helper, "idle"], process::Options { .. }, out, first_err, second_err);
    clean(first_failure);
    assert(first_failure.error == process::Error.LaunchFailed && !first_failure.first_launched && first_failure.second_launched);
    assert(first_failure.first.error == process::Error.LaunchFailed && first_failure.first.os_error == 2 && first_failure.second.signal == 9);
    let second_failure = process::run_pipeline([helper, "idle"], process::Options { .. },
        [missing], process::Options { .. }, out, first_err, second_err);
    clean(second_failure);
    assert(second_failure.error == process::Error.LaunchFailed && !second_failure.first_launched && !second_failure.second_launched);
    assert(second_failure.second.error == process::Error.LaunchFailed && second_failure.second.os_error == 2);

    let prefixes = process::start_pipeline([helper, "prefix"], process::Options { .. },
        [helper, "copy", "0"], process::Options { .. }, owner);
    checked(owner, prefixes.error == process::Error.None);
    let busy = process::start_pipeline([helper, "idle"], process::Options { .. },
        [helper, "idle"], process::Options { .. }, owner);
    checked(owner, busy.error == process::Error.InvalidState);
    let invalid_slice, invalid_done = process::pipeline_step(owner, -1);
    checked(owner, invalid_slice.error == process::Error.InvalidInput && !invalid_done);
    var observed = false;
    for attempt in 100 {
        let progress, done = process::pipeline_step(owner, 20);
        checked(owner, progress.error == process::Error.None && !done);
        observed = process::pipeline_stdout(owner).len == 5 && process::pipeline_first_stderr(owner).len == 6;
        if observed { break; }
    }
    checked(owner, observed);
    let cancelled = process::close_pipeline(owner); clean(cancelled);
    assert(cancelled.error == process::Error.None && cancelled.first.signal == 9 && cancelled.second.signal == 9);
    assert(process::pipeline_stdout(owner) == "ready" && process::pipeline_first_stderr(owner) == "first\n");
    clean(process::close_pipeline(owner));

    var deadline = process::start_pipeline([helper, "idle"], process::Options { timeout_ms: 150 },
        [helper, "idle"], process::Options { timeout_ms: 150 }, owner);
    if deadline.error == process::Error.None { deadline = process::wait_pipeline(owner); }
    clean(deadline); assert(deadline.error == process::Error.TimedOut);
    if deadline.first_launched { assert(deadline.first.signal == 9); }
    if deadline.second_launched { assert(deadline.second.signal == 9); }
    clean(process::close_pipeline(owner));

    reject(helper, marker, process::Options { stdin: process::Input.Stream }, process::Options { .. });
    reject(helper, marker, process::Options { stdout: process::Output.Capture }, process::Options { .. });
    reject(helper, marker, process::Options { stdout_limit: 0 }, process::Options { .. });
    reject(helper, marker, process::Options { stderr: process::Output.Stdout }, process::Options { .. });
    reject(helper, marker, process::Options { capture: false }, process::Options { .. });
    reject(helper, marker, process::Options { .. }, process::Options { stdin: process::Input.Bytes });
    reject(helper, marker, process::Options { .. }, process::Options { stdout: process::Output.Null });
    reject(helper, marker, process::Options { .. }, process::Options { stderr: process::Output.Inherit });
    reject(helper, marker, process::Options { timeout_ms: 1 }, process::Options { timeout_ms: 2 });
    reject(helper, marker, process::Options { timeout_ms: 0 }, process::Options { timeout_ms: 0 });
    let invalid_first = process::run_pipeline([helper, "bad\x00argument"], process::Options { .. },
        [helper, "mark", marker], process::Options { .. }, out, first_err, second_err);
    clean(invalid_first);
    let first_marked = file_exists(marker); if first_marked { assert(delete_file(marker)); }
    assert(invalid_first.error == process::Error.InvalidInput && !invalid_first.first_launched && !invalid_first.second_launched && !first_marked);
    let invalid_second = process::run_pipeline([helper, "mark", marker], process::Options { .. },
        [helper, "bad\x00argument"], process::Options { .. }, out, first_err, second_err);
    clean(invalid_second);
    let second_marked = file_exists(marker); if second_marked { assert(delete_file(marker)); }
    assert(invalid_second.error == process::Error.InvalidInput && !invalid_second.first_launched && !invalid_second.second_launched && !second_marked);
    print("managed pipeline public API tests passed");
}
