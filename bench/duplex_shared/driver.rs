// SPDX-License-Identifier: Apache-2.0
// Diagnostic fixed-workload wrapper around goose-process's SAME native kernel.
use std::env;
use std::path::Path;
use std::time::Instant;

#[repr(C)]
struct Input {
    data: *const u8,
    len: i64,
}
#[repr(C)]
struct Output {
    data: *mut u8,
    len: i64,
}
#[repr(C)]
struct Words {
    data: *mut i64,
    len: i64,
}
unsafe extern "C" {
    fn gp_can_start(state: Words) -> i64;
    fn gp_start(
        packed: Input,
        cwd: Input,
        sets: Input,
        removes: Input,
        input_size: i64,
        config: Words,
        state: Words,
        meta: Words,
    ) -> i64;
    fn gp_io_step(
        state: Words,
        input: Input,
        out: Output,
        err: Output,
        slice_ms: i64,
        meta: Words,
        progress: Words,
    ) -> i64;
    fn gp_close_stdin(state: Words, meta: Words) -> i64;
    fn gp_close(state: Words, meta: Words) -> i64;
    fn gp_id(state: Words) -> i64;
}
fn bytes(value: &[u8]) -> Input {
    Input {
        data: value.as_ptr(),
        len: value.len() as i64,
    }
}
fn output(value: &mut [u8]) -> Output {
    Output {
        data: value.as_mut_ptr(),
        len: value.len() as i64,
    }
}
fn words(value: &mut [i64]) -> Words {
    Words {
        data: value.as_mut_ptr(),
        len: value.len() as i64,
    }
}
fn metadata() -> [i64; 6] {
    [0, -1, 0, 0, 0, 0]
}
fn status(code: i64, meta: &[i64; 6], operation: &str) -> Result<(), String> {
    if meta[5] != 0 {
        return Err(format!(
            "{operation} transferred PID {}: fixed-fixture controller must recover ownership",
            meta[5]
        ));
    }
    if code != 0 {
        return Err(format!("{operation}: code={code}, errno={}", meta[0]));
    }
    Ok(())
}

struct Owner {
    state: [i64; 24],
    closed: bool,
}
impl Owner {
    fn close(&mut self) -> Result<(), String> {
        let mut meta = metadata();
        // SAFETY: live disjoint arrays match the private native ABI. gp_close
        // closes this owner or explicitly transfers a PID through meta[5].
        let code = unsafe { gp_close(words(&mut self.state), words(&mut meta)) };
        self.closed = true;
        status(code, &meta, "close")?;
        if unsafe { gp_id(words(&mut self.state)) } != -1 {
            return Err("closed native owner still has an ID".into());
        }
        Ok(())
    }
}
impl Drop for Owner {
    fn drop(&mut self) {
        if !self.closed {
            if let Err(error) = self.close() {
                // Ordinary oracle failures must not bypass cleanup. Exceptional
                // transferred ownership is a failing fixture-controller boundary.
                eprintln!("native owner cleanup failed: {error}");
            }
        }
    }
}

fn execute(worker: &[u8], frame: &[u8], requests: usize) -> Result<u128, String> {
    let mut owner = Owner {
        state: [0; 24],
        closed: false,
    };
    let mut out = [0u8; 257];
    let mut err = [0u8; 3];
    let ack = b"ack\0";
    let began = Instant::now();
    // Mirror the public fixed-input start path: owner availability, argument
    // bounds/NUL validation and copied NUL-delimited staging are all timed.
    if unsafe { gp_can_start(words(&mut owner.state)) } == 0 {
        return Err("native owner unavailable".into());
    }
    if worker.is_empty() || worker[0] != b'/' || worker.len() > 4095 || worker.contains(&0) {
        return Err("invalid fixed executable path".into());
    }
    let mut packed = Vec::with_capacity(worker.len() + 1);
    packed.extend_from_slice(worker);
    packed.push(0);
    let mut config = [30000, 6, 6, 4, -1, -1, -1, 0, 0, 65536, 65536];
    let mut meta = metadata();
    // SAFETY: all spans point to live, disjoint arrays/vectors; lengths and
    // repr(C) match native/process.h. The engine retains no caller pointers.
    let code = unsafe {
        gp_start(
            bytes(&packed),
            bytes(&[]),
            bytes(&[]),
            bytes(&[]),
            0,
            words(&mut config),
            words(&mut owner.state),
            words(&mut meta),
        )
    };
    drop(packed);
    status(code, &meta, "start")?;
    for _ in 0..requests {
        let (mut sent, mut received, mut acknowledged) = (0, 0, 0);
        while sent < frame.len() || received < frame.len() || acknowledged < 4 {
            let mut meta = metadata();
            let mut progress = [0; 3];
            let offered = &frame[sent..];
            // Mirrors relevant public per-call bounds for this fixed mode.
            assert!(offered.len() <= 1048576 && out.len() <= 1048576 && err.len() <= 1048576);
            let code = unsafe {
                gp_io_step(
                    words(&mut owner.state),
                    bytes(offered),
                    output(&mut out),
                    output(&mut err),
                    20,
                    words(&mut meta),
                    words(&mut progress),
                )
            };
            status(code, &meta, "io_step")?;
            if progress[0] != 0 || progress[2] != 1 {
                return Err("worker completed or closed input before response".into());
            }
            if progress[1] < 0
                || progress[1] > offered.len() as i64
                || meta[3] < 0
                || meta[3] > out.len() as i64
                || meta[3] > (frame.len() - received) as i64
                || meta[4] < 0
                || meta[4] > err.len() as i64
                || meta[4] > (4 - acknowledged) as i64
            {
                return Err("native response counts exceed remaining spans".into());
            }
            let (n, m) = (meta[3] as usize, meta[4] as usize);
            if out[..n] != frame[received..received + n]
                || err[..m] != ack[acknowledged..acknowledged + m]
            {
                return Err("response byte oracle".into());
            }
            sent += progress[1] as usize;
            received += n;
            acknowledged += m;
        }
    }
    let mut meta = metadata();
    let code = unsafe { gp_close_stdin(words(&mut owner.state), words(&mut meta)) };
    status(code, &meta, "close_stdin")?;
    loop {
        let mut meta = metadata();
        let mut progress = [0; 3];
        let code = unsafe {
            gp_io_step(
                words(&mut owner.state),
                bytes(&[]),
                output(&mut out),
                output(&mut err),
                20,
                words(&mut meta),
                words(&mut progress),
            )
        };
        status(code, &meta, "drain")?;
        if progress[1] != 0 || progress[2] != 0 || meta[3] != 0 || meta[4] != 0 {
            return Err("unexpected input progress or trailing output after EOF".into());
        }
        if progress[0] != 0 {
            if meta[1] != 0 || meta[2] != 0 {
                return Err("worker unsuccessful exit".into());
            }
            break;
        }
    }
    owner.close()?;
    Ok(began.elapsed().as_nanos())
}
fn main() {
    let args: Vec<String> = env::args().collect();
    assert!(args.len() == 4 && Path::new(&args[1]).is_absolute());
    let size: usize = args[2].parse().expect("size");
    let requests: usize = args[3].parse().expect("requests");
    assert!((size == 31 || size == 4096) && requests == 128);
    let mut frame = [0u8; 4100];
    frame[2] = (size / 256) as u8;
    frame[3] = (size % 256) as u8;
    for i in 0..size {
        frame[i + 4] = ((i * 17 + i / 251) % 256) as u8;
    }
    match execute(args[1].as_bytes(), &frame[..size + 4], requests) {
        Ok(ns) => println!("{} {} {}", ns, requests * (size + 4), requests * 4),
        Err(error) => {
            eprintln!("{error}");
            std::process::exit(1);
        }
    }
}
