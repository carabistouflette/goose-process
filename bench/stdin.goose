// SPDX-License-Identifier: Apache-2.0
import process from "..";
import std;
import os;
fn main() {
    let argv = args(); assert(argv.len == 3);
    let finite = argv[1] == "finite"; assert(finite || argv[1] == "incremental");
    let iterations, valid = parse_int(argv[2]); assert(valid && iterations >= 1 && iterations <= 100);
    let worker_text = str(resource_dir(), "worker"); let worker: const u8[:] = worker_text[..];
    var batch: u8[>..] = []; batch.resize(524800, 0);
    for frame in 128 {
        let offset = frame * 4100; batch[offset + 2] = 16;
        for i in 4096 { batch[offset + i + 4] = ((frame + i) % 256) as u8; }
    }
    var output: u8[>..] = []; var errors: u8[>..] = [];
    var child = process::Child { .. }; var empty: u8[0] = [];
    for iteration in iterations + 1 {
        let began = clock_ns();
        if finite {
            let status = process::run([worker], process::Options {
                output_limit: 1048576, stdin: process::Input.Bytes, input: batch[..],
            }, output, errors);
            assert(process::success(status) && output == batch && errors.len == 512);
            for i in errors.len { assert(errors[i] == "ack\x00"[i % 4]); }
        } else {
            let status = process::start([worker], process::Options {
                output_limit: 1048576, stdin: process::Input.Stream,
            }, child);
            assert(status.error == process::Error.None);
            var sent: i64 = 0; var done = false; var closed = false;
            while !done {
                let progress = process::io_step(child, batch[sent..], empty[..], empty[..], 20);
                assert(progress.status.error == process::Error.None);
                sent += progress.input_accepted;
                if sent == batch.len && !closed {
                    assert(process::close_stdin(child).error == process::Error.None); closed = true;
                }
                done = progress.done;
                if done { assert(process::success(progress.status)); }
            }
            assert(sent == batch.len && process::stdout(child) == batch && process::stderr(child).len == 512);
            for i in 512 { assert(process::stderr(child)[i] == "ack\x00"[i % 4]); }
            assert(process::close(child).error == process::Error.None);
        }
        print(iteration, " ", clock_ns() - began);
    }
}
