// SPDX-License-Identifier: Apache-2.0
import process from "..";
import std;

fn compile(source: const u8[:], expected_success: bool) {
    var out: u8[>..] = []; var first_err: u8[>..] = []; var second_err: u8[>..] = [];
    let status = process::run_pipeline(
        ["/usr/bin/printf", "%s", source], process::Options { .. },
        ["/usr/bin/cc", "-x", "c", "-fsyntax-only", "-"], process::Options { .. },
        out, first_err, second_err);
    // This fixed example reports only after both owned children are cleaned up.
    // Exceptional transferred PIDs require caller recovery: see CALLERS.md.
    assert(status.first.unreaped_pid == 0 && status.second.unreaped_pid == 0);
    assert(status.error == process::Error.None && status.first_launched && status.second_launched);
    assert(process::success(status.first) && process::pipeline_success(status) == expected_success);
    assert(out.len == 0 && first_err.len == 0);
    if expected_success { assert(second_err.len == 0); }
    else { assert(second_err.len > 0 && status.second.exit_code != 0); }
}
fn main() {
    compile("int main(void) { return 0; }\n", true);
    compile("int main(void) { invalid syntax }\n", false);
    print("pipeline compiler checks passed");
}
