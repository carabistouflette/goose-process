// SPDX-License-Identifier: Apache-2.0
import process from "..";
import std;
import os;

fn main() {
    let helper_text = str(resource_dir(), "helper");
    let helper: const u8[:] = helper_text[..];
    var child = process::Child { .. };
    var out:      u8[4093] = [0; 4093];
    var err:      u8[2053] = [0; 2053];
    var retained: u8[>..]  = []; var errors: u8[>..] = [];
    let rejected = process::run(["/bin/true"], process::Options { stdout: process::Output.Stream }, retained, errors);
    assert(rejected.error == process::Error.InvalidInput);
    var result = process::start([helper, "streams", "3145728"], process::Options {
        stdout: process::Output.Stream, stderr: process::Output.Stream, output_limit: 0,
    }, child);
    assert(result.error == process::Error.None);
    let bad_wait = process::wait(child);
    assert(bad_wait.error == process::Error.InvalidState);
    let bad_step, bad_done = process::step(child);
    assert(bad_step.error == process::Error.InvalidState && !bad_done);
    let invalid, invalid_done, invalid_out, invalid_err = process::stream_step(child, out[..], err[..], -1);
    assert(invalid.error == process::Error.InvalidInput && !invalid_done && invalid_out == 0 && invalid_err == 0);
    let stalled, stalled_done, stalled_out, stalled_err = process::stream_step(child, out[..0], err[..0]);
    assert(stalled.error == process::Error.None && !stalled_done && stalled_out == 0 && stalled_err == 0);
    var total_out: i64 = 0; var total_err: i64 = 0; var done = false;
    while !done {
        let current, complete, n, m = process::stream_step(child, out[..], err[..], 20);
        assert(current.error == process::Error.None);
        for byte in out[..n] { assert(byte == 'x'); }
        for byte in err[..m] { assert(byte == 'y'); }
        total_out += n; total_err += m; done = complete; result = current;
    }
    assert(process::success(result) && total_out == 3145728 && total_err == 3145728);
    assert(process::stdout(child).len == 0 && process::stderr(child).len == 0);
    let repeated, terminal, n, m = process::stream_step(child, out[..], err[..]);
    assert(process::success(repeated) && terminal && n == 0 && m == 0);
    assert(process::close(child).error == process::Error.None);

    // Backpressure one stream while the other and the copied input can progress.
    result = process::start([helper, "bytes"], process::Options {
        stdout: process::Output.Stream, stderr: process::Output.Capture, output_limit: 256,
    }, child);
    assert(result.error == process::Error.None);
    for i in 5 {
        let current, complete, n, m = process::stream_step(child, out[..0], err[..0], 1);
        assert(current.error == process::Error.None && !complete && n == 0 && m == 0);
    }
    done = false; total_out = 0;
    while !done {
        let current, complete, n, m = process::stream_step(child, out[..1], err[..0], 20);
        assert(current.error == process::Error.None && m == 0);
        if n == 1 { assert(out[0] == total_out as u8); }
        total_out += n; done = complete; result = current;
    }
    assert(process::success(result) && total_out == 256 && process::stderr(child).len == 256);
    for i in 256 { assert(process::stderr(child)[i] == (255 - i) as u8); }
    var input: u8[>..] = [];
    for i in 1048576 { input.push((i % 256) as u8); }
    result = process::start(["/bin/cat"], process::Options {
        stdout: process::Output.Stream, stderr: process::Output.Null,
        stdin:  process::Input.Bytes, input: input[..],
    }, child);
    assert(result.error == process::Error.None);
    total_out = 0; done = false;
    while !done {
        let current, complete, n, m = process::stream_step(child, out[..], err[..0], 20);
        assert(current.error == process::Error.None && m == 0);
        for i in n { assert(out[i] == ((total_out + i) % 256) as u8); }
        total_out += n; done = complete; result = current;
    }
    assert(process::success(result) && total_out == input.len);
    result = process::start([helper, "streams", "257"], process::Options {
        stdout: process::Output.Stream, stderr: process::Output.Stdout, output_limit: 0,
    }, child);
    assert(result.error == process::Error.None);
    total_out = 0; done = false;
    while !done {
        let current, complete, n, m = process::stream_step(child, out[..], err[..0], 20);
        assert(current.error == process::Error.None && m == 0);
        for i in n {
            let expected: u8 = if total_out + i < 257 { 'x' } else { 'y' };
            assert(out[i] == expected);
        }
        total_out += n; done = complete; result = current;
    }
    assert(process::success(result) && total_out == 514);
    result = process::start([helper, "streams", "256"], process::Options {
        stdout: process::Output.Stream, stderr: process::Output.Capture, stderr_limit: 255,
    }, child);
    assert(result.error == process::Error.None);
    done = false; total_out = 0;
    while !done {
        let current, complete, n, m = process::stream_step(child, out[..], err[..0], 20);
        for byte in out[..n] { assert(byte == 'x'); }
        total_out += n; done = complete; result = current;
    }
    assert(result.error == process::Error.OutputLimit && total_out == 256 && process::stderr(child).len == 255);
    // Reaping is observationally distinct from pipe completion.
    result = process::start([helper, "bytes"], process::Options {
        stdout: process::Output.Stream, stderr: process::Output.Null,
    }, child);
    assert(result.error == process::Error.None);
    var reaped = false;
    while !reaped {
        let current, complete = process::try_wait(child);
        assert(current.error == process::Error.None);
        reaped = complete;
    }
    assert(process::id(child) == -1);
    total_out = 0; done = false;
    while !done {
        let current, complete, n, m = process::stream_step(child, out[..], err[..0], 20);
        for i in n { assert(out[i] == (total_out + i) as u8); }
        total_out += n; done = complete; result = current;
    }
    assert(process::success(result) && total_out == 256);
    // A capture-only reuse restores legacy wait and capture behavior.
    result = process::start(["/bin/echo", "reuse"], process::Options { .. }, child);
    assert(result.error == process::Error.None && process::success(process::wait(child)));
    assert(process::stdout(child) == "reuse\n");
    print("streaming process API tests passed");
}
