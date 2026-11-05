// SPDX-License-Identifier: Apache-2.0
import process from "..";
import std;

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
    print("incremental stdin process API tests passed");
}
