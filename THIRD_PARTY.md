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
the C compiler, Python, `/bin/true` and `/bin/sleep`; profiling uses `strace`.
Those dependencies retain their respective licenses and are not redistributed
by this repository.
