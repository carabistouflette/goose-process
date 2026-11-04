# Changelog

## Unreleased

First experimental release candidate. The API may change before 1.0.

- Literal byte arguments, explicit cwd, child-environment PATH lookup by opt-in,
  and per-child inherit/clear/set/remove environment policy.
- Independent bounded binary capture, inherited/null/borrowed-FD streams,
  stderr merging, and finite binary stdin with concurrent feed/drain.
- Noncopyable Child ownership with start, bounded step, try_wait, wait, signals,
  terminate, kill and explicit close. Reaped child identities are hidden.
- Finite 30-second defaults. Only explicitly supervised Child handles accept
  timeout_ms=0 for no overall deadline; synchronous run stays finite.
- Child descriptor isolation, default signal restoration, pidfd signaling,
  terminal-status reaping and explicit one-time exceptional ownership transfer.
- Public/ownership tests, independent input oracles, deterministic deadline and
  fault tests, GCC/Clang fortified ASan/UBSan, validation fuzzing and pinned CI.
- Reproducible Rust/Go/Python successful-workload comparisons and a separate
  shared-engine diagnostic; raw samples, toolchains and limitations are retained.

Requires Linux kernel 5.3+, glibc 2.34+ and the compiler in [GOOSE_REVISION](GOOSE_REVISION).
Finite stdin uses a one-way Unix socket, not a FIFO; borrowed FDs allow actual
pipe/terminal/file semantics. The library is not a sandbox or process-tree
supervisor. See [the API](docs/API.md) and [caller contracts](docs/CALLERS.md)
for limits and host duties, and [validation evidence](docs/VALIDATION.md).

A 0.1.0 tag is gated on the coordinated Honk migration and its caller-level
signal/lock/recovery validation. No release is claimed by this candidate entry.
