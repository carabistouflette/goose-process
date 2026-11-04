# Security policy

This library is experimental and has no stable release. Security fixes target
the current default branch and the compiler revision in `GOOSE_REVISION`.
Older commits and other toolchains do not have a maintenance guarantee.

Report suspected vulnerabilities privately using
[GitHub's vulnerability reporting](https://github.com/carabistouflette/goose-process/security/advisories/new).
Include the library/compiler commits, Linux kernel and glibc versions, a minimal
reproducer, expected behavior and relevant diagnostics. Remove credentials and
production data. Allow a fix to be prepared before posting exploit details in
public issues or pull requests.

The [API contract](docs/API.md) defines argument/capture limits, descriptor
isolation, cancellation and exceptional child-ownership transfer. A process
runner is not a sandbox: applications select trusted executables and coordinate
environment/cwd mutation, SIGCHLD disposition and child reaping. Descendants are
not supervised. These boundaries are part of the supported API.

Changes affecting native validation, process ownership or descriptor handling
require independent review, public regression coverage, GCC/Clang sanitizers
and validation fuzzing. Benchmarks must preserve these checks.
