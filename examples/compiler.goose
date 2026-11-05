// SPDX-License-Identifier: Apache-2.0
// Count compiler diagnostics as chunks arrive, without retaining the full log.
import process from "..";
import std;

fn compile(source: const u8[:], expected_success: bool) {
    var child = process::Child { .. };
    let launched = process::start(["/usr/bin/cc", "-x", "c", "-fsyntax-only", "-"], process::Options {
        stdin:  process::Input.Bytes, input: source,
        stdout: process::Output.Null, stderr: process::Output.Stream,
    }, child);
    assert(launched.error == process::Error.None);
    var chunk: u8[1024] = [0; 1024]; var empty: u8[0] = [];
    var done = false; var diagnostic_bytes: i64 = 0; var diagnostic_lines: i64 = 0;
    while !done {
        let status, complete, n, m = process::stream_step(child, empty[..], chunk[..], 20);
        diagnostic_bytes += m;
        for byte in chunk[..m] { if byte == '\n' { diagnostic_lines += 1; } }
        done = complete;
        if status.error != process::Error.None {
            // Exceptional transfer recovery is application policy: see CALLERS.md.
            assert(status.unreaped_pid == 0);
            assert(process::close(child).unreaped_pid == 0);
            assert(false);
        }
        if done { assert(process::success(status) == expected_success); }
    }
    assert(process::close(child).unreaped_pid == 0);
    if expected_success { assert(diagnostic_bytes == 0); }
    else { assert(diagnostic_bytes > 0 && diagnostic_lines > 0); }
}
fn main() {
    compile("int main(void) { return 0; }\n", true);
    compile("int main(void) { return ; invalid syntax }\n", false);
    // Print only after both owned compiler children have been reaped.
    print("streamed compiler diagnostics checked");
}
