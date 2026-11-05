// SPDX-License-Identifier: Apache-2.0
import process from "..";
import std;
import os;

fn main() {
    let argv = args();
    assert(argv.len == 4);
    let size, valid = parse_int(argv[2]);
    let iterations, count_valid = parse_int(argv[3]);
    assert(valid && count_valid && size > 0 && size <= 1048576 && iterations > 0 && iterations <= 100);
    let capture = argv[1] == "capture";
    let chunk = if argv[1] == "stream-4k" { 4096 } else { 16384 };
    assert(capture || argv[1] == "stream-4k" || argv[1] == "stream-16k");
    let helper_text = str(resource_dir(), "helper");
    let helper: const u8[:] = helper_text[..];
    var output: u8[>..]   = []; var errors: u8[>..] = [];
    var out:    u8[16384] = [0; 16384]; var err: u8[16384] = [0; 16384];
    var child = process::Child { .. };
    for i in iterations + 1 {
        let started = clock_ns();
        if capture {
            let status = process::run([helper, "streams", argv[2]], process::Options {
                output_limit: size,
            }, output, errors);
            assert(process::success(status) && output.len == size && errors.len == size);
            for byte in output { assert(byte == 'x'); }
            for byte in errors { assert(byte == 'y'); }
        } else {
            let status = process::start([helper, "streams", argv[2]], process::Options {
                stdout: process::Output.Stream, stderr: process::Output.Stream,
            }, child);
            assert(status.error == process::Error.None);
            var done = false; var total_out: i64 = 0; var total_err: i64 = 0;
            while !done {
                let current, complete, n, m = process::stream_step(child, out[..chunk], err[..chunk], 20);
                assert(current.error == process::Error.None);
                for byte in out[..n] { assert(byte == 'x'); }
                for byte in err[..m] { assert(byte == 'y'); }
                total_out += n; total_err += m; done = complete;
                if done { assert(process::success(current)); }
            }
            assert(total_out == size && total_err == size);
        }
        print(i, " ", clock_ns() - started);
    }
}
