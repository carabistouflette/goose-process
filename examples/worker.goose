// SPDX-License-Identifier: Apache-2.0
import process from "..";
import std;
import os;

fn main() {
    let worker_text = str(resource_dir(), "worker");
    let worker: const u8[:] = worker_text[..];
    var child = process::Child { .. };
    let launched = process::start([worker], process::Options {
        stdout: process::Output.Stream, stderr: process::Output.Stream, stdin: process::Input.Stream,
    }, child);
    assert(launched.error == process::Error.None);
    var frame: u8[4100] = [0; 4100]; var out: u8[257] = [0; 257]; var err: u8[3] = [0; 3];
    var empty: u8[0]    = [];
    // A single worker processes >1 MiB cumulatively; no whole-input buffer/queue.
    for request in 400 {
        let size: i64 = if request % 10 == 0 { 0 } else if request % 10 == 1 { 1 }
            else if request % 10 == 2 { 31 } else { 4096 };
        let frame_size = size + 4;
        frame[0] = 0; frame[1] = 0; frame[2] = (size / 256) as u8; frame[3] = (size % 256) as u8;
        for i in size { frame[i + 4] = ((request + i) % 256) as u8; }
        var sent: i64 = 0; var received: i64 = 0; var acknowledgments: i64 = 0;
        let ack = "ack\x00";
        while sent < frame_size || received < frame_size || acknowledgments < ack.len {
            let progress = process::io_step(child, frame[sent..frame_size], out[..], err[..], 20);
            assert(progress.status.error == process::Error.None && !progress.done && progress.input_open);
            sent += progress.input_accepted;
            assert(received + progress.stdout_count <= frame_size && acknowledgments + progress.stderr_count <= ack.len);
            for i in progress.stdout_count { assert(out[i] == frame[received + i]); }
            for i in progress.stderr_count { assert(err[i] == ack[acknowledgments + i]); }
            received += progress.stdout_count; acknowledgments += progress.stderr_count;
        }
    }
    assert(process::close_stdin(child).error == process::Error.None);
    var done = false;
    while !done {
        let progress = process::io_step(child, empty[..], out[..], err[..], 20);
        assert(progress.status.error == process::Error.None && !progress.input_open && progress.input_accepted == 0);
        assert(progress.stdout_count == 0 && progress.stderr_count == 0);
        done = progress.done;
        if done { assert(process::success(progress.status)); }
    }
    assert(process::close(child).error == process::Error.None);
    print("400 framed worker requests passed");
}
