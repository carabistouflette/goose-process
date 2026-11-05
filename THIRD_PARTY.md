# Third-party and tooling notices

This repository contains no vendored compiler, C library, third-party runtime
binary or bundled system tool. Its library sources are distributed under the
root [Apache-2.0 license](LICENSE).

The setup and native-source fingerprint scripts adapt Apache-2.0 tooling from
[goose-log](https://github.com/carabistouflette/goose-log) and
[goose-json](https://github.com/carabistouflette/goose-json). Their SPDX notices
are retained where present; relevant contributor attributions are recorded in
[NOTICE](NOTICE). No vendored JSON acceleration sources are included here.

Development setup downloads the upstream
[Goose compiler](https://github.com/aardappel/goose) at the full commit recorded
in `GOOSE_REVISION`. Compiler sources/builds live in ignored `.toolchain/` and
are not part of the published library. Runtime process operations use the host
Linux kernel and glibc. Tests/benchmarks additionally invoke host tools such as
the C compiler, Python, `/bin/true` and `/bin/sleep`; optional profiling uses
`strace`, Valgrind/Memcheck and Callgrind. Their generated reports are retained
as evidence; tool executables are not bundled.
Those dependencies retain their respective licenses and are not redistributed
by this repository.

The optional comparison harness also uses the host Rust compiler, Go toolchain
and their standard libraries. No Rust crates, Go modules or Python packages are
downloaded or vendored; the original comparison drivers carry Apache-2.0 notices.

The optional `checker-setup` developer target downloads goose-check at
`CHECKER_REVISION` into ignored build storage and builds it with the shared
pinned compiler. That independent project retains its Apache-2.0 license and
bundled dependency notices. It is not vendored or required by this library's
runtime/build. The consumer uses stable checker source while testing the
current library; see docs/CHECKER-CONSUMER.md.
