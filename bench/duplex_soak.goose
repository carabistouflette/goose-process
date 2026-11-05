// SPDX-License-Identifier: Apache-2.0
import process from "..";
import std;
import os;

fn chunk(seed: i64&) -> i64 {
    seed = seed * 48271 % 2147483647;
    let sizes = [1, 7, 255, 4093, 16384];
    return sizes[seed % 5];
}
fn main() {
    let helper_text = str(resource_dir(), "helper"); let helper: const u8[:] = helper_text[..];
    let worker_text = str(resource_dir(), "worker"); let worker: const u8[:] = worker_text[..];
    var child = process::Child { .. };
    var out:   u8[16384] = [0; 16384]; var err: u8[16384] = [0; 16384];
    var input: u8[65536] = [0; 65536]; for i in input.len { input[i] = (i % 256) as u8; }
    var frame: u8[4100]  = [0; 4100]; var empty: u8[0] = [];
    var line:  u8[>..]   = []; var index: i64 = 0; var seed: i64 = 20261105;
    print("ready"); flush_stdout();
    while read_line(line) {
        if line == "stop" { print("stopped ", index); flush_stdout(); return; }
        assert(line == "next");
        let kind = index % 6; let began = clock_ns();
        var status = process::Status { .. };
        if kind == 0 {
            status = process::start([worker], process::Options {
                stdin: process::Input.Stream, stdout: process::Output.Stream, stderr: process::Output.Stream,
            }, child);
        } else if kind == 1 || kind == 5 {
            status = process::start([helper, "duplex-burst"], process::Options {
                stdin:  process::Input.Stream, stdout: process::Output.Stream,
                stderr: if kind == 5 { process::Output.Capture } else { process::Output.Stream }, stderr_limit: 127,
            }, child);
        } else if kind == 2 {
            status = process::start([helper, "close-input"], process::Options {
                stdin: process::Input.Stream, stdout: process::Output.Stream, stderr: process::Output.Stream,
            }, child);
        } else if kind == 3 {
            status = process::start(["/bin/cat"], process::Options {
                stdin: process::Input.Stream, stdout: process::Output.Stream, stderr: process::Output.Stream, timeout_ms: 0,
            }, child);
        } else {
            status = process::start([helper, "sleep", "60000"], process::Options {
                stdin: process::Input.Stream, stdout: process::Output.Stream, stderr: process::Output.Stream, timeout_ms: 50,
            }, child);
        }
        assert(status.error == process::Error.None);
        var sent:         i64 = 0; var received: i64 = 0; var errors: i64 = 0; var calls: i64 = 0;
        var empty_offers: i64 = 0; var idle_accepts: i64 = 0; var signaled = false;
        var closed = false; var done = false; var peer_closed = false;
        if kind == 0 {
            // One live worker handles >1MiB. Validate before reusing each frame.
            let ack = "ack\x00";
            for request in 400 {
                let size: i64 = if request % 10 == 0 { 0 } else if request % 10 == 1 { 1 }
                    else if request % 10 == 2 { 31 } else { 4096 };
                let length = size + 4;
                frame[0] = 0; frame[1] = 0; frame[2] = (size / 256) as u8; frame[3] = (size % 256) as u8;
                for i in size { frame[i + 4] = ((request + i) % 256) as u8; }
                var accepted: i64 = 0; var echoed: i64 = 0; var acks: i64 = 0;
                while accepted < length || echoed < length || acks < 4 {
                    let offer_size = min(chunk(seed), length - accepted);
                    let pause_offer = calls % 17 == 0;
                    let offer = if pause_offer { empty[..] } else { frame[accepted..accepted + offer_size] };
                    let progress = process::io_step(child, offer,
                        out[..if calls % 19 == 0 { 0 } else { chunk(seed) }],
                        err[..if calls % 23 == 0 { 0 } else { chunk(seed) }], if pause_offer { 0 } else { 20 });
                    assert(progress.input_accepted >= 0 && progress.input_accepted <= offer.len && progress.input_accepted <= 16384);
                    if offer.len == 0 { empty_offers += 1; assert(progress.input_accepted == 0); }
                    if offer.len > 0 && progress.input_accepted == 0 { idle_accepts += 1; }
                    assert(echoed + progress.stdout_count <= length && acks + progress.stderr_count <= 4);
                    for i in progress.stdout_count { assert(out[i] == frame[echoed + i]); }
                    for i in progress.stderr_count { assert(err[i] == ack[acks + i]); }
                    accepted += progress.input_accepted; echoed += progress.stdout_count; acks += progress.stderr_count;
                    sent += progress.input_accepted; received += progress.stdout_count; errors += progress.stderr_count; calls += 1;
                    assert(progress.status.error == process::Error.None && !progress.done && progress.input_open);
                    if calls % 64 == 0 { sleep_ms(1); }
                }
            }
            assert(sent == 1149760 && received == sent && errors == 1600);
            assert(process::close_stdin(child).error == process::Error.None);
            assert(process::close_stdin(child).error == process::Error.None); closed = true;
        }
        while !done {
            let wanted = if kind == 1 { 2097152 } else { 1048576 };
            let offer_size = min(chunk(seed), min(input.len - sent % input.len, max(0, wanted - sent)));
            let offer = if closed || sent >= wanted || calls % 17 == 0 { empty[..] }
                else { input[sent % input.len..sent % input.len + offer_size] };
            let progress = process::io_step(child, offer, out[..chunk(seed)],
                if kind == 5 { empty[..] } else { err[..chunk(seed)] }, 20);
            assert(progress.input_accepted >= 0 && progress.input_accepted <= offer.len && progress.input_accepted <= 16384);
            if kind == 2 {
                if peer_closed { assert(!progress.input_open && progress.input_accepted == 0); }
                if !progress.input_open { peer_closed = true; }
            }
            if offer.len == 0 { empty_offers += 1; assert(progress.input_accepted == 0); }
            if offer.len > 0 && progress.input_accepted == 0 { idle_accepts += 1; }
            for i in progress.stdout_count {
                let offset = received + i;
                if kind == 0 || kind == 4 { assert(false); }
                else if kind == 1 || kind == 5 {
                    let expected: u8 = if offset < 262144 { 'x' } else { ((offset - 262144) % 256) as u8 };
                    assert(out[i] == expected);
                } else if kind == 2 { assert(offset < 6 && out[i] == "closed"[offset]); }
                else { assert(out[i] == (offset % 256) as u8); }
            }
            for i in progress.stderr_count { assert(kind == 1 && err[i] == 'y'); }
            sent += progress.input_accepted; received += progress.stdout_count; errors += progress.stderr_count; calls += 1;
            status = progress.status; done = progress.done;
            assert(status.unreaped_pid == 0);
            if !done {
                assert(status.error == process::Error.None);
                if kind == 1 && sent == wanted && !closed {
                    assert(process::close_stdin(child).error == process::Error.None); closed = true;
                }
                if (kind == 2 && !progress.input_open && received == 6) || (kind == 3 && received > 0 && calls >= 5) {
                    if !signaled { assert(process::terminate(child).error == process::Error.None); signaled = true; }
                }
            } else { assert(!progress.input_open); }
            if kind == 1 && calls % 64 == 0 { sleep_ms(1); }
        }
        if kind == 0 { assert(process::success(status) && sent == received && errors == 1600); }
        if kind == 1 { assert(process::success(status) && sent == 2097152 && received == sent + 262144 && errors == 262144); }
        if kind == 2 { assert(status.error == process::Error.None && status.signal == 15 && signaled && received == 6 && errors == 0); }
        if kind == 3 { assert(status.error == process::Error.None && status.signal == 15 && signaled && received <= sent && errors == 0); }
        if kind == 4 { assert(status.error == process::Error.TimedOut && received == 0 && errors == 0); }
        if kind == 5 {
            assert(status.error == process::Error.OutputLimit && process::stderr(child).len == 127);
            for byte in process::stderr(child) { assert(byte == 'y'); }
        }
        assert(process::id(child) == -1);
        let terminal = process::io_step(child, input[..], out[..], if kind == 5 { empty[..] } else { err[..] });
        assert(terminal.done && terminal.status.error == status.error && terminal.status.exit_code == status.exit_code &&
            terminal.status.signal == status.signal && terminal.status.unreaped_pid == 0 && !terminal.input_open &&
            terminal.input_accepted == 0 && terminal.stdout_count == 0 && terminal.stderr_count == 0);
        let closed_status = process::close(child);
        assert(closed_status.error == status.error && closed_status.unreaped_pid == 0);
        print("case ", index, " ", kind, " ", clock_ns() - began, " ", calls, " ", sent, " ", received, " ", errors,
            " ", status.exit_code, " ", status.signal, " ", if status.error == process::Error.TimedOut { 4 }
            else if status.error == process::Error.OutputLimit { 5 } else { 0 }, " ", empty_offers, " ", idle_accepts);
        flush_stdout(); index += 1; line.clear();
    }
    assert(false);
}
