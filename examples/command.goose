// SPDX-License-Identifier: Apache-2.0
import process from "..";
import std;
fn main() {
    var out: u8[>..] = [];
    var err: u8[>..] = [];
    let status = process::run(["/usr/bin/printf", "%s", "hello from Goose"], process::Options { .. }, out, err);
    if process::success(status) { print(out); }
    else { print("command failed: ", status.error, ", OS error ", status.os_error); }
}
