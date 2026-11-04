// SPDX-License-Identifier: Apache-2.0
// Diagnostic Rust front end to the SAME native kernel as Goose. This is not
// an independent Rust process implementation or std::process benchmark.
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
    fn gp_execute(
        packed: Input,
        cwd: Input,
        sets: Input,
        removes: Input,
        input: Input,
        config: Words,
        out: Output,
        err: Output,
        meta: Words,
    ) -> i64;
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

fn main() {
    let argv: Vec<String> = std::env::args().collect();
    assert_eq!(argv.len(), 4);
    let mode = &argv[2];
    let count: u32 = argv[3].parse().unwrap();
    assert!(count > 0);
    let mut command = vec![argv[1].as_bytes(), mode.as_bytes()];
    if mode == "args" {
        for _ in 0..128 {
            command.push(b"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
        }
    }
    let input = vec![b'x'; if mode == "input" { 131072 } else { 0 }];
    let expected = vec![
        b'x';
        match mode.as_str() {
            "input" => 131072,
            "streams" | "redirect" => 65536,
            _ => 0,
        }
    ];
    let errors = vec![b'y'; if mode == "streams" { 65536 } else { 0 }];
    let limit = if mode == "input" { 131072 } else { 65536 };
    let mut out = Vec::new();
    let mut err = Vec::new();
    let mut invoke = || {
        // Mirror public staging, validation and capture capacity reuse. Native
        // validation remains enabled and is shared verbatim with Goose.
        assert!(!command.is_empty() && command.len() <= 4096);
        assert!(!command[0].is_empty() && command[0][0] == b'/' && command[0].len() <= 4095);
        let mut size = 0;
        for arg in &command {
            assert!(arg.len() <= 131071 && !arg.contains(&0));
            size += arg.len() + 1;
            assert!(size <= 1048576);
        }
        let mut packed = Vec::new();
        for arg in &command {
            packed.extend_from_slice(arg);
            packed.push(0);
        }
        let sets = if mode == "environment" {
            b"PROBE=goose\0".to_vec()
        } else {
            Vec::new()
        };
        let input = input.clone();
        out.clear();
        out.resize(limit, 0);
        err.clear();
        if mode != "redirect" {
            err.resize(limit, 0);
        }
        let mut config = [
            10000,
            1,
            if mode == "redirect" { 3 } else { 1 },
            if mode == "input" { 2 } else { 0 },
            -1,
            -1,
            -1,
            if mode == "environment" { 1 } else { 0 },
            0,
            limit as i64,
            limit as i64,
        ];
        let mut meta = [0, -1, 0, 0, 0, 0];
        // SAFETY: all pointer/length pairs reference live, disjoint owned
        // vectors/arrays for the entire synchronous call. repr(C) matches
        // native/process.h; gp_execute retains no payload pointers.
        let code = unsafe {
            gp_execute(
                bytes(&packed),
                bytes(&[]),
                bytes(&sets),
                bytes(&[]),
                bytes(&input),
                words(&mut config),
                output(&mut out),
                output(&mut err),
                words(&mut meta),
            )
        };
        assert_eq!(code, 0);
        assert_eq!(meta[1], 0);
        assert_eq!(meta[5], 0);
        assert!(meta[3] >= 0 && meta[3] <= out.len() as i64);
        assert!(meta[4] >= 0 && meta[4] <= err.len() as i64);
        out.truncate(meta[3] as usize);
        err.truncate(meta[4] as usize);
        assert_eq!(out, expected);
        assert_eq!(err, errors);
    };
    for _ in 0..3 {
        invoke();
    }
    let start = Instant::now();
    for _ in 0..count {
        invoke();
    }
    println!("{}", start.elapsed().as_nanos() / count as u128);
}
