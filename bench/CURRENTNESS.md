# Which measurements describe current code?

This audit compares the recorded source revisions with published implementation
`7f759bb22e4afcbe65c78dee20861509c3310a52`. Documentation changes after that
revision do not change the implementation. [Machine-readable checks](CURRENTNESS.json)
list every retained report's hash and measured/current runtime Git objects.

## Current evidence

The [pipeline study](PIPELINES.md) measured clean
`396ada2b8d9a55a22d405a77833f4a03e6aa889d`. All 12 recorded source files match
7f759bb exactly: public and native code, build manifest, compiler pin, benchmark
driver/controller and helper. The recorded Goose and GCC executable SHA-256s
also match the installed binaries inspected in this audit. Pre/post identities
in the raw report agree. No timing rerun was needed for this unchanged code.

The table measures complete API routes through the same engine. It is useful
for choosing convenience copies or borrowed captures for those two workloads;
it is not an isolated copy-cost measurement or a comparison with other libraries.
The full report keeps machine/OS/toolchain, units, warmups, shuffled repeats,
raw timings, dispersion, slower results and shared-host limits.

You can inspect the source bridge without running a benchmark:

```sh
git diff --exit-code 396ada2b8d9a55a22d405a77833f4a03e6aa889d 7f759bb22e4afcbe65c78dee20861509c3310a52 -- process.goose process.build GOOSE_REVISION native bench/pipeline.goose bench/pipeline_measure.py tests/pipeline_helper.c scripts/checker_consumer.py scripts/native-manifest.py
```

## Historical evidence

Every other library timing/profile/soak family predates runtime changes in
`process.goose`, `process.build` or `native/process.c`. Recent recording dates do
not make those results current. The old reports remain intact, with their source
and tool identities, methods, caveats and all raw artifacts. Use the
[historical index](README.md#earlier-studies); do not quote those tables as
current-main performance.

The Honk records use consumer `453aa53dc7b20622782423c64cc180097f653650`.
The checker-overhead record uses checker `32de6c4783bf8d3242e042993dadd160a98b2945`,
not current checker `713e2749ded459b915f59012aaa6e19351a17b18`. These are
historical consumer results, not measurements of today's applications.

The inventory contains 33 JSON-object reports and one profiler-control list.
An independent audit checked 254 declared source-hash mappings against their
named Git revisions with no mismatches. The machine-readable audit separately
compares runtime Git objects and preserves each raw report's SHA-256. Compiler
pins in the named reports agree on `eadca409f68210893f366e60d0c2f0dc29a339be`;
that agreement does not undo later library changes.

No new cross-language comparison is needed to describe the API. No current
cross-language speed ranking, peak-memory claim or sustained no-leak claim is
made. New measurements would need a defined question and a fresh finite protocol,
rather than rerunning historical tables merely to give them a new date.
