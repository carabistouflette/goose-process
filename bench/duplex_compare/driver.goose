// SPDX-License-Identifier: Apache-2.0
import process from "../..";
import std;
import os;

fn checked(child: process::Child&, valid: bool) {
    if !valid {
        // The fixed-fixture benchmark must still clean up a failed oracle.
        let closed = process::close(child);
        assert(closed.unreaped_pid == 0);
        assert(false);
    }
}

fn main() {
    let argv = args(); assert(argv.len == 4 && argv[1].len > 0 && argv[1][0] == '/');
    let size, valid_size = parse_int(argv[2]);
    let requests, valid_requests = parse_int(argv[3]);
    assert(valid_size && (size == 31 || size == 4096) && valid_requests && requests == 128);
    var frame: u8[4100] = [0; 4100]; var out: u8[257] = [0; 257]; var err: u8[3] = [0; 3];
    var empty: u8[0]    = [];
    frame[2] = (size / 256) as u8; frame[3] = (size % 256) as u8;
    for i in size { frame[i + 4] = ((i * 17 + i / 251) % 256) as u8; }
    let width = size + 4; let ack = "ack\x00";
    var child = process::Child { .. };
    let worker: const u8[:] = argv[1];
    let began = clock_ns();
    let launched = process::start([worker], process::Options {
        timeout_ms: 30000, stdin: process::Input.Stream,
        stdout:     process::Output.Stream, stderr: process::Output.Stream,
    }, child);
    assert(launched.error == process::Error.None);
    for request in requests {
        var sent: i64 = 0; var received: i64 = 0; var acknowledged: i64 = 0;
        while sent < width || received < width || acknowledged < 4 {
            let progress = process::io_step(child, frame[sent..width], out[..], err[..], 20);
            checked(child, progress.status.error == process::Error.None && !progress.done && progress.input_open);
            checked(child, progress.input_accepted <= width - sent && progress.stdout_count <= width - received && progress.stderr_count <= 4 - acknowledged);
            for i in progress.stdout_count { checked(child, out[i] == frame[received + i]); }
            for i in progress.stderr_count { checked(child, err[i] == ack[acknowledged + i]); }
            sent += progress.input_accepted; received += progress.stdout_count; acknowledged += progress.stderr_count;
        }
    }
    checked(child, process::close_stdin(child).error == process::Error.None);
    var done = false;
    while !done {
        let progress = process::io_step(child, empty[..], out[..], err[..], 20);
        checked(child, progress.status.error == process::Error.None && progress.input_accepted == 0 && !progress.input_open);
        checked(child, progress.stdout_count == 0 && progress.stderr_count == 0);
        done = progress.done;
        if done { checked(child, process::success(progress.status)); }
    }
    assert(process::close(child).error == process::Error.None && process::id(child) == -1);
    print(clock_ns() - began, " ", requests * width, " ", requests * 4);
}
