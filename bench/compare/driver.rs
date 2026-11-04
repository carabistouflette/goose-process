// SPDX-License-Identifier: Apache-2.0
// Standard-library output baseline; no timeout or hard capture cap.
use std::io::Write;
use std::process::{Command, Stdio};
use std::time::Instant;
fn main() {
    let args: Vec<String> = std::env::args().collect();
    let fixture = &args[1];
    let mode = &args[2];
    let count: u32 = args[3].parse().unwrap();
    let arguments = if mode == "args" {
        vec!["x".repeat(32); 128]
    } else {
        vec![]
    };
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
    let invoke = || {
        let mut command = Command::new(fixture);
        command.arg(mode).args(&arguments).stdout(Stdio::piped());
        command.stderr(if mode == "redirect" {
            Stdio::null()
        } else {
            Stdio::piped()
        });
        command.stdin(if mode == "input" {
            Stdio::piped()
        } else {
            Stdio::null()
        });
        if mode == "environment" {
            command.env_clear().env("PROBE", "goose");
        }
        let mut child = command.spawn().unwrap();
        // Feed concurrently: sequential writes can deadlock on a full stdout pipe.
        let input = &input;
        std::thread::scope(|scope| {
            let writer = child
                .stdin
                .take()
                .map(|mut stdin| scope.spawn(move || stdin.write_all(input).unwrap()));
            let output = child.wait_with_output().unwrap();
            if let Some(thread) = writer {
                thread.join().unwrap();
            }
            assert!(output.status.success());
            assert_eq!(output.stdout, expected);
            assert_eq!(output.stderr, errors);
        });
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
