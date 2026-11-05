// SPDX-License-Identifier: Apache-2.0
import process from "..";
import std;
import os;

fn chunk(seed: i64&) -> i64 {
    seed = seed * 48271 % 2147483647;
    let sizes = [1, 7, 255, 4093, 16384];
    return sizes[seed % 5];
}
fn pattern(position: i64) -> u8 { return ((position * 17 + position / 251) % 256) as u8; }
fn main() {
    let helper_text = str(resource_dir(), "helper");
    let helper: const u8[:] = helper_text[..];
    var child = process::Child { .. };
    var out:   u8[16384] = [0; 16384]; var err: u8[16384] = [0; 16384];
    var input: u8[>..]   = []; for i in 1048576 { input.push((i % 256) as u8); }
    var line:  u8[>..]   = []; var index: i64 = 0; var seed: i64 = 20261105;
    print("ready"); flush_stdout();
    while read_line(line) {
        if line == "stop" { print("stopped ", index); flush_stdout(); return; }
        assert(line == "next" || line == "finite");
        let finite_kinds = [0, 1, 4, 5];
        let kind = if line == "finite" { finite_kinds[index % 4] } else { index % 6 };
        let began = clock_ns();
        var status = process::Status { .. };
        if kind == 0 {
            status = process::start([helper, "binary-streams", "3145728"], process::Options {
                stdout: process::Output.Stream, stderr: process::Output.Stream,
            }, child);
        } else if kind == 1 {
            status = process::start(["/bin/cat"], process::Options {
                stdout: process::Output.Stream, stderr: process::Output.Stream,
                stdin:  process::Input.Bytes, input: input[..],
            }, child);
        } else if kind == 2 {
            status = process::start([helper, "flood"], process::Options {
                stdout: process::Output.Stream, stderr: process::Output.Stream,
            }, child);
        } else if kind == 3 {
            status = process::start([helper, "bytes"], process::Options {
                stdout: process::Output.Stream, stderr: process::Output.Stream,
            }, child);
        } else if kind == 4 {
            status = process::start(["/bin/true"], process::Options {
                stdout: process::Output.Stream, stderr: process::Output.Stream,
                stdin:  process::Input.Bytes, input: input[..],
            }, child);
        } else {
            status = process::start([helper, "bytes"], process::Options {
                stdout: process::Output.Stream, stderr: process::Output.Capture, stderr_limit: 256,
            }, child);
        }
        assert(status.error == process::Error.None);
        var total_out: i64 = 0; var total_err: i64 = 0; var calls: i64 = 0; var done = false;
        var already_reaped = false; var signal_code: i64 = -1; var signal_errno: i64 = 0;
        if kind == 3 {
            if index % 12 == 3 {
                sleep_ms(2); // Exercise already-reaped observations as well as immediate races.
                let observed, reaped = process::try_wait(child);
                assert(observed.error == process::Error.None); already_reaped = reaped;
            }
            let sent = process::terminate(child);
            signal_code = if sent.error == process::Error.None { 0 } else if sent.error == process::Error.InvalidState { 6 } else { 3 };
            signal_errno = sent.os_error;
            assert(sent.error == process::Error.None ||
                (sent.error == process::Error.InvalidState && already_reaped && process::id(child) == -1) ||
                (sent.error == process::Error.SystemError && sent.os_error == 3 && !already_reaped && process::id(child) > 0));
        }
        while !done {
            let out_size = chunk(seed); let err_size = if kind == 5 { 0 } else { chunk(seed) };
            let current, complete, n, m = process::stream_step(child, out[..out_size], err[..err_size], 20);
            assert(current.error == process::Error.None && current.unreaped_pid == 0);
            for i in n {
                let expected: u8 = if kind == 0 { pattern(total_out + i) }
                    else if kind == 1 { ((total_out + i) % 256) as u8 }
                    else if kind == 3 || kind == 5 { (total_out + i) as u8 } else { 'x' };
                assert(out[i] == expected);
            }
            for i in m {
                let expected: u8 = if kind == 0 { 255 - pattern(total_err + i) }
                    else if kind == 3 { (255 - total_err - i) as u8 }
                    else if kind == 2 { 'x' } else { 'y' };
                assert(err[i] == expected);
            }
            total_out += n; total_err += m; calls += 1; done = complete; status = current;
            if kind == 0 && calls % 32 == 0 { sleep_ms(1); }
            if kind == 2 && calls == 5 { assert(process::terminate(child).error == process::Error.None); }
        }
        if kind == 0 { assert(process::success(status) && total_out == 3145728 && total_err == 3145728); }
        if kind == 1 { assert(process::success(status) && total_out == input.len && total_err == 0); }
        if kind == 2 { assert(status.signal == 15); }
        if kind == 3 {
            assert((status.exit_code == 0 && status.signal == 0) || status.signal == 15);
            if status.exit_code == 0 { assert(total_out == 256 && total_err == 256); }
        }
        if kind == 4 { assert(process::success(status) && total_out == 0 && total_err == 0); }
        if kind == 5 {
            assert(process::success(status) && total_out == 256 && total_err == 0);
            assert(process::stderr(child).len == 256);
            for i in 256 { assert(process::stderr(child)[i] == (255 - i) as u8); }
        }
        assert(process::id(child) == -1);
        let closed = process::close(child);
        assert(closed.error == process::Error.None && closed.unreaped_pid == 0);
        print("case ", index, " ", kind, " ", clock_ns() - began, " ", calls, " ", total_out, " ", total_err,
              " ", status.exit_code, " ", status.signal, " ", if already_reaped { 1 } else { 0 },
              " ", signal_code, " ", signal_errno);
        flush_stdout(); index += 1; line.clear();
    }
    assert(false);
}
