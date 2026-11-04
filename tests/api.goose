// SPDX-License-Identifier: Apache-2.0
import process from "..";
import std;
import os;

fn main() {
    let helper_text = str(resource_dir(), "helper");
    let helper: const u8[:] = helper_text[..];
    var out: u8[>..] = [];
    var err: u8[>..] = [];
    var options = process::Options { .. };
    var status = process::run([helper[..], "args", "", "a b", "\";$(false)*"], options, out, err);
    assert(process::success(status));
    assert(out == "\x00a b\x00\";$(false)*\x00" && err.len == 0);
    status = process::run([helper[..], "bytes"], options, out, err);
    assert(process::success(status) && out.len == 256 && err.len == 256);
    for i in 256 { assert(out[i] == i && err[i] == 255 - i); }
    for code in [0, 1, 126, 127] {
        let text_storage = str(code);
        let text: const u8[:] = text_storage[..];
        status = process::run([helper[..], "exit", text[..]], options, out, err);
        assert(status.error == process::Error.None && status.exit_code == code && status.signal == 0);
        assert(process::success(status) == (code == 0));
    }
    status = process::run([helper[..], "signal"], options, out, err);
    assert(status.error == process::Error.None && status.exit_code == -1 && status.signal == 15);
    options.timeout_ms = 250;
    status = process::run([helper[..], "trace"], options, out, err);
    if status.exit_code == 77 { assert(process::success(status)); }
    else { assert(status.error == process::Error.TimedOut && status.signal == 9 && status.unreaped_pid == 0); }
    options.timeout_ms = 30000;
    status = process::run([helper[..], "stdin"], options, out, err);
    assert(process::success(status));
    status = process::run(["/no/such/goose-process-executable"], options, out, err);
    assert(status.error == process::Error.LaunchFailed && status.os_error > 0 && status.exit_code == -1);
    options.cwd = "/tmp";
    status = process::run([helper[..], "cwd"], options, out, err);
    assert(process::success(status) && out == "/tmp");
    options.cwd = "/no/such/goose-process-directory";
    status = process::run([helper[..], "exit"], options, out, err);
    assert(status.error == process::Error.LaunchFailed);
    options.cwd = "";
    options.output_limit = 32768;
    status = process::run([helper[..], "streams", "32768"], options, out, err);
    assert(process::success(status) && out.len == 32768 && err.len == 32768);
    status = process::run([helper[..], "streams", "32769"], options, out, err);
    assert(status.error == process::Error.OutputLimit && out.len <= 32768 && err.len <= 32768);
    options.output_limit = 0;
    status = process::run([helper[..], "exit"], options, out, err);
    assert(process::success(status) && out.len == 0 && err.len == 0);
    status = process::run([helper[..], "args", "x"], options, out, err);
    assert(status.error == process::Error.OutputLimit && out.len == 0);
    options.timeout_ms = 20;
    status = process::run([helper[..], "sleep"], options, out, err);
    assert(status.error == process::Error.TimedOut);
    status = process::run([helper[..], "descendant"], options, out, err);
    assert(status.error == process::Error.TimedOut);
    options.capture = false;
    status = process::run([helper[..], "exit"], options, out, err);
    assert(process::success(status) && out.len == 0 && err.len == 0);
    options = process::Options { .. };
    status = process::run([""], options, out, err);
    assert(status.error == process::Error.InvalidInput);
    status = process::run(["true"], options, out, err);
    assert(status.error == process::Error.InvalidInput);
    status = process::run(["/bin/true\x00x"], options, out, err);
    assert(status.error == process::Error.InvalidInput);
    var empty: (const u8[:])[>..] = [];
    status = process::run(empty, options, out, err);
    assert(status.error == process::Error.InvalidInput);
    options.timeout_ms = 0;
    out.append("old"); err.append("old");
    status = process::run([helper[..]], options, out, err);
    assert(status.error == process::Error.InvalidInput && out.len == 0 && err.len == 0);
    options = process::Options { .. };
    status = process::run(["/bin/true"], options, out, out);
    assert(status.error == process::Error.InvalidInput && out.len == 0);
    var many: (const u8[:])[>..] = [];
    many.push("/bin/true");
    for i in 255 { many.push(""); }
    status = process::run(many, options, out, err);
    assert(process::success(status));
    many.push("");
    status = process::run(many, options, out, err);
    assert(status.error == process::Error.InvalidInput);
    var long_text: u8[>..] = [];
    long_text.resize(16384, 'x');
    let long_view: const u8[:] = long_text[..];
    status = process::run(["/bin/true", long_view], options, out, err);
    assert(process::success(status));
    for limit in [-1, 1048577] {
        options.output_limit = limit;
        status = process::run(["/bin/true"], options, out, err);
        assert(status.error == process::Error.InvalidInput);
    }
    options.output_limit = 65536;
    options.cwd = "bad\x00directory";
    status = process::run(["/bin/true"], options, out, err);
    assert(status.error == process::Error.InvalidInput);
    print("process API tests passed");
}
