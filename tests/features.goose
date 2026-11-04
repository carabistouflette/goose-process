// SPDX-License-Identifier: Apache-2.0
import process from "..";
import std;

fn main() {
    var out: u8[>..] = [];
    var err: u8[>..] = [];
    let changes = [process::EnvOverride { key: "PROBE", value: "value" }];
    var result = process::run(["/usr/bin/env"], process::Options {
        clear_environment: true, environment: changes[..],
    }, out, err);
    assert(process::success(result) && out == "PROBE=value\n" && err.len == 0);
    let path = [process::EnvOverride { key: "PATH", value: "/bin:/usr/bin" }];
    result = process::run(["echo", "hello"], process::Options {
        clear_environment: true, environment: path[..], search_path: true,
    }, out, err);
    assert(process::success(result) && out == "hello\n");
    result = process::run(["/bin/cat"], process::Options {
        stdin: process::Input.Bytes, input: "binary\x00\xffinput",
    }, out, err);
    assert(process::success(result) && out == "binary\x00\xffinput");
    result = process::run(["/usr/bin/printf", "discard"], process::Options {
        stdout: process::Output.Null, stderr: process::Output.Capture,
    }, out, err);
    assert(process::success(result) && out.len == 0 && err.len == 0);
    var child = process::Child { .. };
    result = process::start(["/bin/sleep", "60"], process::Options { timeout_ms: 1000 }, child);
    assert(result.error == process::Error.None && process::id(child) > 0);
    let observed, done = process::try_wait(child);
    assert(observed.error == process::Error.None && !done);
    let second = process::start(["/bin/true"], process::Options { .. }, child);
    assert(second.error == process::Error.InvalidState);
    assert(process::terminate(child).error == process::Error.None);
    result = process::wait(child);
    assert(result.error == process::Error.None && result.signal == 15 && result.exit_code == -1);
    assert(process::close(child).error == process::Error.None);
    assert(process::close(child).unreaped_pid == 0);
    var input: u8[>..] = [];
    for i in 131072 { input.push((i % 256) as u8); }
    result = process::start(["/bin/cat"], process::Options {
        output_limit: 131072, stdin: process::Input.Bytes, input: input[..],
    }, child);
    assert(result.error == process::Error.None);
    var finished = false;
    while !finished {
        let current, complete = process::step(child, 20);
        assert(current.error == process::Error.None);
        finished = complete;
    }
    assert(process::stdout(child) == input && process::stderr(child).len == 0);
    assert(process::success(process::wait(child)));
    // Input views are copied before output replacement, including child reuse.
    out.clear(); out.append("/bin/echo");
    let command: const u8[:] = out[..];
    result = process::run([command, "reuse"], process::Options { .. }, out, err);
    assert(process::success(result) && out == "reuse\n");
    let previous = process::stdout(child);
    result = process::start(["/bin/cat"], process::Options {
        output_limit: 131072, stdin: process::Input.Bytes, input: previous,
    }, child);
    assert(result.error == process::Error.None);
    assert(process::success(process::wait(child)) && process::stdout(child) == input);
    let invalid, invalid_done = process::step(child, -1);
    assert(invalid.error == process::Error.InvalidInput && !invalid_done);
    assert(process::stdout(child) == input);
    result = process::run(["/bin/true"], process::Options { timeout_ms: 0 }, out, err);
    assert(result.error == process::Error.InvalidInput && out.len == 0 && err.len == 0);
    result = process::start(["/bin/sleep", "60"], process::Options { timeout_ms: 0 }, child);
    assert(result.error == process::Error.None && process::id(child) > 0);
    let running, completed = process::step(child, 0);
    assert(running.error == process::Error.None && !completed);
    assert(process::terminate(child).error == process::Error.None);
    result = process::wait(child);
    assert(result.error == process::Error.None && result.signal == 15);
    result = process::start(["/bin/cat"], process::Options {
        timeout_ms: 0, stdin: process::Input.Bytes, input: "supervised",
    }, child);
    assert(result.error == process::Error.None);
    assert(process::success(process::wait(child)) && process::stdout(child) == "supervised");
    print("extended process API tests passed");
}
