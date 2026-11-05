// SPDX-License-Identifier: Apache-2.0
import process from "..";
import std;
import os;

fn main() {
    var child = process::Child { .. };
    var out:   u8[257]   = [0; 257]; var empty: u8[0] = [];
    var input: u8[65536] = [0; 65536]; for i in input.len { input[i] = (i % 256) as u8; }
    let launched = process::start(["/bin/cat"], process::Options {
        stdout: process::Output.Stream, stderr: process::Output.Null, stdin: process::Input.Stream,
    }, child);
    assert(launched.error == process::Error.None);
    let idle = process::io_step(child, empty[..], out[..], empty[..]);
    assert(idle.status.error == process::Error.None && idle.input_open && !idle.done && idle.input_accepted == 0);
    assert(process::wait(child).error == process::Error.InvalidState);
    let old, old_done, old_out, old_err = process::stream_step(child, out[..], empty[..]);
    assert(old.error == process::Error.InvalidState && !old_done);
    var sent: i64 = 0; var received: i64 = 0; var done = false; var closed = false;
    while !done {
        let offset = sent % input.len;
        let offered = if sent < 3145728 { input[offset..] } else { empty[..] };
        let current = process::io_step(child, offered, out[..], empty[..], 20);
        assert(current.status.error == process::Error.None && current.input_accepted <= offered.len);
        assert(current.input_accepted <= 16384 && current.stderr_count == 0);
        for i in current.stdout_count { assert(out[i] == ((received + i) % 256) as u8); }
        sent += current.input_accepted; received += current.stdout_count;
        if !closed {
            assert(current.input_open);
            if sent == 3145728 {
                assert(process::close_stdin(child).error == process::Error.None);
                assert(process::close_stdin(child).error == process::Error.None);
                closed = true;
            }
        } else { assert(!current.input_open && current.input_accepted == 0); }
        done = current.done;
        if done { assert(process::success(current.status)); }
    }
    assert(sent == 3145728 && received == sent && process::id(child) == -1);
    let terminal = process::io_step(child, input[..], out[..], empty[..]);
    assert(terminal.done && process::success(terminal.status) && !terminal.input_open && terminal.input_accepted == 0 && terminal.stdout_count == 0);
    assert(process::close(child).error == process::Error.None);
    var result = process::start(["/bin/true"], process::Options {
        stdout: process::Output.Null, stderr: process::Output.Null, stdin: process::Input.Stream,
    }, child);
    assert(result.error == process::Error.None);
    done = false;
    while !done {
        let current = process::io_step(child, input[..], empty[..], empty[..], 20);
        assert(current.status.error == process::Error.None);
        done = current.done;
        if done { assert(process::success(current.status) && !current.input_open); }
    }
    result = process::start(["/bin/cat"], process::Options { stdin: process::Input.Stream }, child);
    assert(result.error == process::Error.None && process::close_stdin(child).error == process::Error.None);
    done = false;
    while !done {
        let current = process::io_step(child, input[..], empty[..], empty[..], 20);
        assert(current.status.error == process::Error.None && !current.input_open && current.input_accepted == 0);
        done = current.done;
        if done { assert(process::success(current.status)); }
    }
    assert(process::stdout(child).len == 0);
    result = process::start(["/bin/echo", "finite"], process::Options { .. }, child);
    assert(result.error == process::Error.None && process::success(process::wait(child)));
    assert(process::stdout(child) == "finite\n" && process::close_stdin(child).error == process::Error.InvalidState);
    var output: u8[>..] = []; var errors: u8[>..] = [];
    result = process::run(["/bin/true"], process::Options { stdin: process::Input.Stream }, output, errors);
    assert(result.error == process::Error.InvalidInput);
    result = process::start(["/bin/cat"], process::Options { stdin: process::Input.Stream }, child);
    assert(result.error == process::Error.None);
    var oversized: u8[>..] = []; oversized.resize(process::MAX_INPUT_BYTES + 1, 0);
    let rejected_offer = process::io_step(child, oversized[..], empty[..], empty[..]);
    assert(rejected_offer.status.error == process::Error.InvalidInput && rejected_offer.input_accepted == 0);
    let rejected_slice = process::io_step(child, empty[..], empty[..], empty[..], -1);
    assert(rejected_slice.status.error == process::Error.InvalidInput);
    let captured = process::io_step(child, "a\x00\xff", empty[..], empty[..], 20);
    assert(captured.status.error == process::Error.None && captured.input_open && captured.input_accepted == 3);
    assert(captured.stdout_count == 0 && captured.stderr_count == 0);
    assert(process::close_stdin(child).error == process::Error.None);
    done = false;
    while !done {
        let current = process::io_step(child, empty[..], empty[..], empty[..], 20);
        assert(current.status.error == process::Error.None && current.stdout_count == 0 && current.stderr_count == 0);
        done = current.done;
        if done { assert(process::success(current.status)); }
    }
    assert(process::stdout(child) == "a\x00\xff" && process::stderr(child).len == 0);
    let helper_text = str(resource_dir(), "helper"); let helper: const u8[:] = helper_text[..];
    result = process::start([helper, "duplex-burst"], process::Options {
        stdout:       process::Output.Stream, stderr: process::Output.Capture, stdin: process::Input.Stream,
        stderr_limit: 262144,
    }, child);
    assert(result.error == process::Error.None);
    sent = 0; received = 0; closed = false; done = false; var backpressured = false;
    while !done {
        let offered = if sent < 1048576 { input[sent % input.len..] } else { empty[..] };
        let current = process::io_step(child, offered, out[..], empty[..], 20);
        assert(current.status.error == process::Error.None);
        if offered.len > 0 && current.input_accepted == 0 && current.input_open && current.stdout_count > 0 {
            backpressured = true;
        }
        for i in current.stdout_count {
            let expected: u8 = if received + i < 262144 { 'x' } else { ((received + i - 262144) % 256) as u8 };
            assert(out[i] == expected);
        }
        sent += current.input_accepted; received += current.stdout_count;
        if sent == 1048576 && !closed {
            assert(process::close_stdin(child).error == process::Error.None); closed = true;
        }
        done = current.done;
        if done { assert(process::success(current.status)); }
    }
    assert(backpressured && sent == 1048576 && received == 1310720 && process::stderr(child).len == 262144);
    for byte in process::stderr(child) { assert(byte == 'y'); }
    result = process::start([helper, "bytes"], process::Options {
        stdout:       process::Output.Stream, stderr: process::Output.Capture, stdin: process::Input.Stream,
        stderr_limit: 0,
    }, child);
    assert(result.error == process::Error.None);
    done = false;
    while !done {
        let current = process::io_step(child, input[..], out[..], empty[..], 20);
        done = current.done;
        if done { assert(current.status.error == process::Error.OutputLimit && !current.input_open); }
        else { assert(current.status.error == process::Error.None); }
    }
    assert(process::stderr(child).len == 0 && process::id(child) == -1);
    result = process::start([helper, "bytes"], process::Options {
        stdout: process::Output.Stream, stderr: process::Output.Null, stdin: process::Input.Stream,
    }, child);
    assert(result.error == process::Error.None);
    var reaped = false;
    while !reaped {
        let observed, complete = process::try_wait(child);
        assert(observed.error == process::Error.None); reaped = complete;
    }
    assert(process::id(child) == -1);
    done = false; received = 0;
    while !done {
        let current = process::io_step(child, input[..], out[..], empty[..], 20);
        assert(current.status.error == process::Error.None && !current.input_open && current.input_accepted == 0);
        for i in current.stdout_count { assert(out[i] == (received + i) as u8); }
        received += current.stdout_count; done = current.done;
        if done { assert(process::success(current.status)); }
    }
    assert(received == 256);
    result = process::start([helper, "close-input"], process::Options {
        stdout: process::Output.Stream, stderr: process::Output.Null, stdin: process::Input.Stream,
    }, child);
    assert(result.error == process::Error.None);
    var peer_closed = false; received = 0;
    while !peer_closed || received < 6 {
        let current = process::io_step(child, input[..], out[..], empty[..], 20);
        assert(current.status.error == process::Error.None && !current.done);
        for i in current.stdout_count { assert(out[i] == "closed"[received + i]); }
        received += current.stdout_count; peer_closed = !current.input_open;
    }
    assert(process::terminate(child).error == process::Error.None);
    done = false;
    while !done {
        let current = process::io_step(child, input[..], out[..], empty[..], 20);
        assert(current.status.error == process::Error.None && !current.input_open && current.input_accepted == 0);
        done = current.done;
        if done { assert(current.status.signal == 15); }
    }
    print("incremental stdin process API tests passed");
}
