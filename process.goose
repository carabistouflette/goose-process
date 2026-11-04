// SPDX-License-Identifier: Apache-2.0
namespace process;
import std;

enum Error { None, InvalidInput, LaunchFailed, SystemError, TimedOut, OutputLimit }
struct Options {
    cwd:          const u8[:] = "",
    capture:      bool        = true,
    timeout_ms:   i64         = 30000,
    output_limit: i64         = 65536,
}
struct Status {
    error:        Error = Error.None,
    os_error:     i64   = 0,
    exit_code:    i64   = -1,
    signal:       i64   = 0,
    unreaped_pid: i64   = 0,
}
const MAX_ARGUMENTS = 256;
const MAX_ARGUMENT_BYTES = 65536;
const MAX_ARGUMENT_LENGTH = 16384;
const MAX_PATH_BYTES = 4095;
const MAX_OUTPUT_BYTES = 1048576;
const MAX_TIMEOUT_MS = 86400000;

extern fn native_run(packed: const u8[:], cwd: const u8[:], timeout_ms: i64,
                    capture: i64, out: u8[:], err: u8[:], meta: i64[:]) -> i64 = "gp_run";

fn success(status: Status) -> bool {
    return status.error == Error.None && status.exit_code == 0;
}

fn valid_text(text: const u8[:], limit: i64) -> bool {
    if text.len > limit { return false; }
    for c in text { if c == 0 { return false; } }
    return true;
}

// Both output buffers are replaced, including on validation failure. Inputs may
// not borrow these buffers; Goose enforces the input/output lifetime separation.
fn run(args: const (const u8[:])[:], options: Options, out: u8[>..]&, err: u8[>..]&) -> Status {
    out.clear(); err.clear();
    var invalid = Status { error: Error.InvalidInput };
    if args.len == 0 || args.len > MAX_ARGUMENTS ||
       options.timeout_ms < 1 || options.timeout_ms > MAX_TIMEOUT_MS ||
       options.output_limit < 0 || options.output_limit > MAX_OUTPUT_BYTES ||
       !valid_text(options.cwd, MAX_PATH_BYTES) { return invalid; }
    if args[0].len == 0 || args[0][0] != '/' || !valid_text(args[0], MAX_PATH_BYTES) { return invalid; }
    var total: i64 = 0;
    for arg in args {
        if !valid_text(arg, MAX_ARGUMENT_LENGTH) { return invalid; }
        total += arg.len + 1;
        if total > MAX_ARGUMENT_BYTES { return invalid; }
    }
    var packed: u8[>..] = [];
    for arg in args { packed.append(arg); packed.push(0); }
    if options.capture { out.resize(options.output_limit, 0); err.resize(options.output_limit, 0); }
    var meta: i64[6] = [0, -1, 0, 0, 0, 0];
    let code = native_run(packed, options.cwd, options.timeout_ms,
                          if options.capture { 1 } else { 0 }, out[..], err[..], meta[..]);
    out.resize(meta[3]); err.resize(meta[4]);
    let error = if code == 0 { Error.None }
                else if code == 1 { Error.InvalidInput }
                else if code == 2 { Error.LaunchFailed }
                else if code == 3 { Error.SystemError }
                else if code == 4 { Error.TimedOut }
                else { Error.OutputLimit };
    return Status { error: error, os_error: meta[0], exit_code: meta[1], signal: meta[2], unreaped_pid: meta[5] };
}
