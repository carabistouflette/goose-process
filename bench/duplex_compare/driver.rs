// SPDX-License-Identifier: Apache-2.0
use std::env;
use std::io::{self, Read, Write};
use std::os::fd::{AsRawFd, FromRawFd, OwnedFd};
use std::path::Path;
use std::process::{Child, Command, Stdio};
use std::time::{Duration, Instant};

#[repr(C)]
struct PollFd {
    fd: i32,
    events: i16,
    revents: i16,
}
unsafe extern "C" {
    fn poll(fds: *mut PollFd, count: usize, timeout: i32) -> i32;
    fn fcntl(fd: i32, command: i32, ...) -> i32;
    fn pidfd_open(pid: i32, flags: u32) -> i32;
}
const READ: i16 = 1;
const WRITE: i16 = 4;
const HUP: i16 = 16;
const ERR: i16 = 8;
fn nonblocking(fd: i32) -> io::Result<()> {
    // Linux-only F_GETFL/F_SETFL and O_NONBLOCK; preserve every existing flag.
    let flags = unsafe { fcntl(fd, 3) };
    if flags < 0 || unsafe { fcntl(fd, 4, flags | 2048) } < 0 {
        return Err(io::Error::last_os_error());
    }
    Ok(())
}
fn fail(message: &str) -> io::Error {
    io::Error::other(message)
}
fn retry(error: &io::Error) -> bool {
    matches!(
        error.kind(),
        io::ErrorKind::WouldBlock | io::ErrorKind::Interrupted
    )
}
fn wait(descriptors: &mut [PollFd], deadline: Instant) -> io::Result<()> {
    let remaining = deadline
        .checked_duration_since(Instant::now())
        .ok_or_else(|| fail("worker deadline"))?;
    let timeout = remaining.as_nanos().div_ceil(1_000_000).min(20) as i32;
    let result = unsafe { poll(descriptors.as_mut_ptr(), descriptors.len(), timeout) };
    if result < 0 {
        let error = io::Error::last_os_error();
        if !retry(&error) {
            return Err(error);
        }
    }
    Ok(())
}
struct Owner {
    child: Child,
    reaped: bool,
}
impl Drop for Owner {
    fn drop(&mut self) {
        if !self.reaped {
            // This driver exclusively owns the unreaped Child; no external waiter.
            let _ = self.child.kill();
            let _ = self.child.wait();
        }
    }
}
fn execute(worker: &str, frame: &[u8], requests: usize) -> io::Result<u128> {
    let mut out_buffer = [0u8; 257];
    let mut err_buffer = [0u8; 3];
    let ack = b"ack\0";
    let began = Instant::now();
    let deadline = began + Duration::from_secs(30);
    let child = Command::new(worker)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()?;
    let mut owner = Owner {
        child,
        reaped: false,
    };
    let raw_pidfd = unsafe { pidfd_open(owner.child.id() as i32, 0) };
    if raw_pidfd < 0 {
        return Err(io::Error::last_os_error());
    }
    let pidfd = unsafe { OwnedFd::from_raw_fd(raw_pidfd) };
    let mut input = owner
        .child
        .stdin
        .take()
        .ok_or_else(|| fail("missing stdin"))?;
    let mut output = owner
        .child
        .stdout
        .take()
        .ok_or_else(|| fail("missing stdout"))?;
    let mut errors = owner
        .child
        .stderr
        .take()
        .ok_or_else(|| fail("missing stderr"))?;
    for fd in [input.as_raw_fd(), output.as_raw_fd(), errors.as_raw_fd()] {
        nonblocking(fd)?;
    }
    for _ in 0..requests {
        let (mut sent, mut received, mut acknowledged) = (0, 0, 0);
        while sent < frame.len() || received < frame.len() || acknowledged < 4 {
            if Instant::now() >= deadline {
                return Err(fail("worker deadline"));
            }
            let mut descriptors = [
                PollFd {
                    fd: input.as_raw_fd(),
                    events: if sent < frame.len() { WRITE } else { 0 },
                    revents: 0,
                },
                PollFd {
                    fd: output.as_raw_fd(),
                    events: READ,
                    revents: 0,
                },
                PollFd {
                    fd: errors.as_raw_fd(),
                    events: READ,
                    revents: 0,
                },
            ];
            wait(&mut descriptors, deadline)?;
            if sent < frame.len() && descriptors[0].revents & (WRITE | HUP | ERR) != 0 {
                match input.write(&frame[sent..]) {
                    Ok(0) => return Err(fail("zero stdin progress")),
                    Ok(count) => sent += count,
                    Err(error) if retry(&error) => (),
                    Err(error) => return Err(error),
                }
            }
            if descriptors[1].revents & (READ | HUP | ERR) != 0 {
                match output.read(&mut out_buffer) {
                    Ok(0) => return Err(fail("early stdout EOF")),
                    Ok(count) => {
                        if count > frame.len() - received
                            || out_buffer[..count] != frame[received..received + count]
                        {
                            return Err(fail("stdout oracle"));
                        }
                        received += count;
                    }
                    Err(error) if retry(&error) => (),
                    Err(error) => return Err(error),
                }
            }
            if descriptors[2].revents & (READ | HUP | ERR) != 0 {
                match errors.read(&mut err_buffer) {
                    Ok(0) => return Err(fail("early stderr EOF")),
                    Ok(count) => {
                        if count > 4 - acknowledged
                            || err_buffer[..count] != ack[acknowledged..acknowledged + count]
                        {
                            return Err(fail("stderr oracle"));
                        }
                        acknowledged += count;
                    }
                    Err(error) if retry(&error) => (),
                    Err(error) => return Err(error),
                }
            }
        }
    }
    drop(input);
    let (mut out_open, mut err_open) = (true, true);
    while out_open || err_open || !owner.reaped {
        if Instant::now() >= deadline {
            return Err(fail("worker EOF deadline"));
        }
        if !owner.reaped {
            if let Some(status) = owner.child.try_wait()? {
                owner.reaped = true;
                if !status.success() {
                    return Err(fail("worker nonzero exit"));
                }
            }
        }
        if !out_open && !err_open && owner.reaped {
            break;
        }
        let mut descriptors = [
            PollFd {
                fd: if out_open { output.as_raw_fd() } else { -1 },
                events: READ,
                revents: 0,
            },
            PollFd {
                fd: if err_open { errors.as_raw_fd() } else { -1 },
                events: READ,
                revents: 0,
            },
            PollFd {
                fd: if owner.reaped { -1 } else { pidfd.as_raw_fd() },
                events: READ,
                revents: 0,
            },
        ];
        wait(&mut descriptors, deadline)?;
        if out_open && descriptors[0].revents & (READ | HUP | ERR) != 0 {
            match output.read(&mut out_buffer) {
                Ok(0) => out_open = false,
                Ok(_) => return Err(fail("excess stdout")),
                Err(error) if retry(&error) => (),
                Err(error) => return Err(error),
            }
        }
        if err_open && descriptors[1].revents & (READ | HUP | ERR) != 0 {
            match errors.read(&mut err_buffer) {
                Ok(0) => err_open = false,
                Ok(_) => return Err(fail("excess stderr")),
                Err(error) if retry(&error) => (),
                Err(error) => return Err(error),
            }
        }
    }
    drop(pidfd);
    drop(output);
    drop(errors);
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
    match execute(&args[1], &frame[..size + 4], requests) {
        Ok(ns) => println!("{} {} {}", ns, requests * (size + 4), requests * 4),
        Err(error) => {
            eprintln!("{error}");
            std::process::exit(1);
        }
    }
}
