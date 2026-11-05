// SPDX-License-Identifier: Apache-2.0
namespace process;
import std;

enum Error { None, InvalidInput, LaunchFailed, SystemError, TimedOut, OutputLimit, InvalidState }
enum Input { Null, Inherit, Bytes, Fd }
enum Output { Default, Capture, Inherit, Null, Stdout, Fd, Stream }
struct EnvOverride { key: const u8[:], value: const u8[:] = "", remove: bool = false }
const NO_ENVIRONMENT: EnvOverride[0] = [];
struct Options {
    cwd:               const u8[:]          = "",
    capture:           bool                 = true,
    timeout_ms:        i64                  = 30000,
    output_limit:      i64                  = 65536,
    stdout:            Output               = Output.Default,
    stderr:            Output               = Output.Default,
    stdin:             Input                = Input.Null,
    input:             const u8[:]          = "",
    stdout_fd:         i64                  = -1,
    stderr_fd:         i64                  = -1,
    stdin_fd:          i64                  = -1,
    stdout_limit:      i64                  = -1,
    stderr_limit:      i64                  = -1,
    clear_environment: bool                 = false,
    environment:       const EnvOverride[:] = NO_ENVIRONMENT[..],
    search_path:       bool                 = false,
}
struct Status {
    error:        Error = Error.None,
    os_error:     i64   = 0,
    exit_code:    i64   = -1,
    signal:       i64   = 0,
    unreaped_pid: i64   = 0,
}
// Compiler-enforced noncopyable resource owner. Its fields are private API.
root struct Child {
    _native:     i64[24] = [0; 24],
    _input:      u8[>..] = [],
    _stdout:     u8[>..] = [],
    _stderr:     u8[>..] = [],
    _out_size:   i64     = 0,
    _err_size:   i64     = 0,
    _stream_out: bool    = false,
    _stream_err: bool    = false,
}
const MAX_ARGUMENTS = 4096;
const MAX_ARGUMENT_BYTES = 1048576;
const MAX_ARGUMENT_LENGTH = 131071;
const MAX_PATH_BYTES = 4095;
const MAX_OUTPUT_BYTES = 1048576;
const MAX_INPUT_BYTES = 1048576;
const MAX_TIMEOUT_MS = 86400000;
const MAX_ENVIRONMENT_CHANGES = 256;
const MAX_ENVIRONMENT_BYTES = 1048576;
const MAX_ENVIRONMENT_KEY = 256;

extern fn native_execute(packed: const u8[:], cwd: const u8[:], sets: const u8[:], removes: const u8[:],
    input: const u8[:], settings: const i64[:], out: u8[:], err: u8[:], meta: i64[:]) -> i64 = "gp_execute";
extern fn native_start(packed: const u8[:], cwd: const u8[:], sets: const u8[:], removes: const u8[:],
    input_size: i64, settings: const i64[:], state: i64[:], meta: i64[:]) -> i64 = "gp_start";
extern fn native_wait(state: i64[:], input: const u8[:], out: u8[:], err: u8[:],
    slice_ms: i64, one_step: i64, meta: i64[:], done: i64&) -> i64 = "gp_wait";
extern fn native_stream_step(state: i64[:], input: const u8[:], out: u8[:], err: u8[:],
    slice_ms: i64, meta: i64[:], done: i64&) -> i64 = "gp_stream_step";
extern fn native_try_wait(state: i64[:], meta: i64[:], done: i64&) -> i64 = "gp_try_wait";
extern fn native_signal(state: i64[:], signal: i64, meta: i64[:]) -> i64 = "gp_signal";
extern fn native_close(state: i64[:], meta: i64[:]) -> i64 = "gp_close";
extern fn native_can_start(state: i64[:]) -> i64 = "gp_can_start";
extern fn native_id(state: i64[:]) -> i64 = "gp_id";

fn success(status: Status) -> bool { return status.error == Error.None && status.exit_code == 0; }
fn valid_text(text: const u8[:], limit: i64) -> bool {
    if text.len > limit { return false; }
    for c in text { if c == 0 { return false; } }
    return true;
}
fn output_code(mode: Output, capture: bool) -> i64 {
    if mode == Output.Default { return if capture { 1 } else { 2 }; }
    if mode == Output.Capture { return 1; }
    if mode == Output.Inherit { return 2; }
    if mode == Output.Null { return 3; }
    if mode == Output.Stdout { return 4; }
    if mode == Output.Stream { return 6; }
    return 5;
}
fn input_code(mode: Input) -> i64 {
    if mode == Input.Null { return 0; }
    if mode == Input.Inherit { return 1; }
    if mode == Input.Bytes { return 2; }
    return 3;
}
fn limits(options: Options) -> i64, i64 {
    return if options.stdout_limit == -1 { options.output_limit } else { options.stdout_limit },
           if options.stderr_limit == -1 { options.output_limit } else { options.stderr_limit };
}
fn settings(options: Options) -> i64[11] {
    let out_limit, err_limit = limits(options);
    return [options.timeout_ms, output_code(options.stdout, options.capture),
        output_code(options.stderr, options.capture), input_code(options.stdin),
        options.stdout_fd, options.stderr_fd, options.stdin_fd,
        if options.clear_environment { 1 } else { 0 }, if options.search_path { 1 } else { 0 }, out_limit, err_limit];
}
fn valid_fd(mode: bool, fd: i64) -> bool { return if mode { fd >= 0 && fd <= 2147483647 } else { fd == -1 }; }
fn pack(args: const (const u8[:])[:], options: Options,
        packed: u8[>..]&, sets: u8[>..]&, removes: u8[>..]&, allow_unbounded: bool = false) -> bool {
    if args.len == 0 || args.len > MAX_ARGUMENTS || options.timeout_ms < 0 ||
        (options.timeout_ms == 0 && !allow_unbounded) ||
        options.timeout_ms > MAX_TIMEOUT_MS || !valid_text(options.cwd, MAX_PATH_BYTES) ||
        options.environment.len > MAX_ENVIRONMENT_CHANGES || options.stdout == Output.Stdout ||
        !valid_fd(options.stdin == Input.Fd, options.stdin_fd) ||
        !valid_fd(options.stdout == Output.Fd, options.stdout_fd) ||
        !valid_fd(options.stderr == Output.Fd, options.stderr_fd) ||
        !valid_text(args[0], MAX_PATH_BYTES) || args[0].len == 0 ||
        (!options.search_path && args[0][0] != '/') ||
        options.input.len > MAX_INPUT_BYTES || (options.stdin != Input.Bytes && options.input.len != 0) ||
        options.output_limit < 0 || options.output_limit > MAX_OUTPUT_BYTES ||
        options.stdout_limit < -1 || options.stderr_limit < -1 { return false; }
    let out_limit, err_limit = limits(options);
    if out_limit < 0 || out_limit > MAX_OUTPUT_BYTES || err_limit < 0 || err_limit > MAX_OUTPUT_BYTES { return false; }
    var total: i64 = 0;
    for arg in args {
        if !valid_text(arg, MAX_ARGUMENT_LENGTH) { return false; }
        total += arg.len + 1;
        if total > MAX_ARGUMENT_BYTES { return false; }
    }
    for arg in args { packed.append(arg); packed.push(0); }
    var environment_bytes: i64 = 0;
    for change in options.environment {
        if change.key.len == 0 || change.key.len > MAX_ENVIRONMENT_KEY ||
            !valid_text(change.key, MAX_ENVIRONMENT_KEY) || !valid_text(change.value, MAX_ARGUMENT_LENGTH) ||
            (!change.remove && change.key.len + 1 + change.value.len > MAX_ARGUMENT_LENGTH) ||
            (change.remove && change.value.len != 0) { return false; }
        for c in change.key { if c == '=' { return false; } }
        environment_bytes += change.key.len + 1;
        if !change.remove { environment_bytes += change.value.len + 1; }
        if environment_bytes > MAX_ENVIRONMENT_BYTES { return false; }
        if change.remove { removes.append(change.key); removes.push(0); }
        else { sets.append(change.key); sets.push('='); sets.append(change.value); sets.push(0); }
    }
    return true;
}
fn status(code: i64, meta: const i64[:]) -> Status {
    let error = if code == 0 { Error.None } else if code == 1 { Error.InvalidInput }
        else if code == 2 { Error.LaunchFailed } else if code == 3 { Error.SystemError }
        else if code == 4 { Error.TimedOut } else if code == 5 { Error.OutputLimit } else { Error.InvalidState };
    return Status { error: error, os_error: meta[0], exit_code: meta[1], signal: meta[2], unreaped_pid: meta[5] };
}
fn capture_buffers(config: const i64[:], out: u8[>..]&, err: u8[>..]&) {
    if config[1] == 1 { out.resize(config[9], 0); }
    if config[2] == 1 { err.resize(config[10], 0); }
}
fn run(args: const (const u8[:])[:], options: Options, out: u8[>..]&, err: u8[>..]&) -> Status {
    var packed: u8[>..] = []; var sets: u8[>..] = []; var removes: u8[>..] = [];
    if options.stdout == Output.Stream || options.stderr == Output.Stream || !pack(args, options, packed, sets, removes) {
        out.clear(); err.clear(); return Status { error: Error.InvalidInput };
    }
    let config = settings(options);
    var directory: u8[>..] = []; directory.append(options.cwd);
    var input:     u8[>..] = []; input.append(options.input);
    out.clear(); err.clear();
    capture_buffers(config[..], out, err);
    var meta: i64[6] = [0, -1, 0, 0, 0, 0];
    let code = native_execute(packed, directory, sets, removes, input, config[..], out[..], err[..], meta[..]);
    out.resize(meta[3]); err.resize(meta[4]);
    return status(code, meta[..]);
}
fn start(args: const (const u8[:])[:], options: Options, child: Child&) -> Status {
    if native_can_start(child._native[..]) == 0 { return Status { error: Error.InvalidState }; }
    var packed: u8[>..] = []; var sets: u8[>..] = []; var removes: u8[>..] = [];
    if !pack(args, options, packed, sets, removes, true) { return Status { error: Error.InvalidInput }; }
    let config = settings(options);
    var directory: u8[>..] = []; directory.append(options.cwd);
    var input:     u8[>..] = []; input.append(options.input);
    let stream_out = options.stdout == Output.Stream;
    let stream_err = options.stderr == Output.Stream;
    child._input.clear(); child._input.append(input);
    child._stdout.clear(); child._stderr.clear(); child._out_size = 0; child._err_size = 0;
    child._stream_out = stream_out;
    child._stream_err = stream_err;
    capture_buffers(config[..], child._stdout, child._stderr);
    var meta: i64[6] = [0, -1, 0, 0, 0, 0];
    let code = native_start(packed, directory, sets, removes, child._input.len, config[..], child._native[..], meta[..]);
    return status(code, meta[..]);
}
fn collect(child: Child&, slice_ms: i64, one_step: bool) -> Status, bool {
    if child._stream_out || child._stream_err { return Status { error: Error.InvalidState }, false; }
    var meta: i64[6] = [0, -1, 0, 0, 0, 0]; var done: i64 = 0;
    let code = native_wait(child._native[..], child._input[..], child._stdout[..], child._stderr[..],
        slice_ms, if one_step { 1 } else { 0 }, meta[..], done);
    child._out_size = meta[3]; child._err_size = meta[4];
    if done != 0 {
        child._stdout.resize(meta[3]); child._stderr.resize(meta[4]); child._input.clear();
    }
    return status(code, meta[..]), done != 0;
}
fn wait(child: Child&) -> Status { let result, done = collect(child, 0, false); return result; }
fn step(child: Child&, timeout_ms: i64 = 0) -> Status, bool {
    if timeout_ms < 0 || timeout_ms > MAX_TIMEOUT_MS { return Status { error: Error.InvalidInput }, false; }
    return collect(child, timeout_ms, true);
}
// Counts describe only caller Stream spans in this call, including terminal errors.
fn stream_step(child: Child&, out: u8[:], err: u8[:], timeout_ms: i64 = 0) -> Status, bool, i64, i64 {
    if timeout_ms < 0 || timeout_ms > MAX_TIMEOUT_MS || out.len > MAX_OUTPUT_BYTES || err.len > MAX_OUTPUT_BYTES ||
        (!child._stream_out && out.len != 0) || (!child._stream_err && err.len != 0) {
        return Status { error: Error.InvalidInput }, false, 0, 0;
    }
    if !child._stream_out && !child._stream_err { return Status { error: Error.InvalidState }, false, 0, 0; }
    var meta: i64[6] = [0, -1, 0, 0, 0, 0]; var done: i64 = 0;
    let output = if child._stream_out { out } else { child._stdout[..] };
    let errors = if child._stream_err { err } else { child._stderr[..] };
    let code = native_stream_step(child._native[..], child._input[..], output, errors, timeout_ms, meta[..], done);
    if code == 1 || code == 6 { return status(code, meta[..]), false, 0, 0; }
    child._out_size = if child._stream_out { 0 } else { meta[3] };
    child._err_size = if child._stream_err { 0 } else { meta[4] };
    if done != 0 {
        child._stdout.resize(child._out_size); child._stderr.resize(child._err_size); child._input.clear();
    }
    return status(code, meta[..]), done != 0,
        if child._stream_out { meta[3] } else { 0 }, if child._stream_err { meta[4] } else { 0 };
}
fn try_wait(child: Child&) -> Status, bool {
    var meta: i64[6] = [0, -1, 0, 0, 0, 0]; var done: i64 = 0;
    let code = native_try_wait(child._native[..], meta[..], done);
    return status(code, meta[..]), done != 0;
}
fn send_signal(child: Child&, signal: i64) -> Status {
    var meta: i64[6] = [0, -1, 0, 0, 0, 0];
    let code = native_signal(child._native[..], signal, meta[..]);
    return status(code, meta[..]);
}
fn terminate(child: Child&) -> Status { return send_signal(child, 15); }
fn kill(child: Child&) -> Status { return send_signal(child, 9); }
fn close(child: Child&) -> Status {
    var meta: i64[6] = [0, -1, 0, 0, 0, 0];
    let code = native_close(child._native[..], meta[..]);
    child._out_size = if child._stream_out { 0 } else { meta[3] };
    child._err_size = if child._stream_err { 0 } else { meta[4] };
    child._stdout.resize(child._out_size); child._stderr.resize(child._err_size); child._input.clear();
    return status(code, meta[..]);
}
fn id(child: Child&) -> i64 { return native_id(child._native[..]); }
fn stdout(child: Child&) -> const u8[:] { return child._stdout[..child._out_size]; }
fn stderr(child: Child&) -> const u8[:] { return child._stderr[..child._err_size]; }
