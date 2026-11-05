# Benchmarks

The [pipeline study](PIPELINES.md) matches the current implementation through a
checked source bridge. It compares two API routes through the same engine:
convenience copies and borrowed captures. It does not rank process libraries.

```sh
make setup
python3 bench/pipeline_measure.py --goose .toolchain/goose/build/goose --cc gcc --probe-only
```

This builds and checks four cases without making a timing claim. To measure,
commit the sources first and follow the [full protocol](PIPELINES.md). Results
include exact bytes/status checks, machine/tool versions, warmups, all samples
and dispersion. Shared-host measurements are not isolated by CPU pinning.

## Earlier studies

These reports remain available for reproducibility. Their measured code differs
from current main; use the named source/compiler/dependency revisions to reproduce
historical results. Running a harness on main produces a new measurement, not a
reproduction of the old table. Slower outcomes and failed attempts are retained.

| Historical study | Report |
| --- | --- |
| Single-command launch/capture | [Process benchmark](SPAWN-HISTORY.md) |
| Native API versus standard libraries | [Rust/Go/Python](compare/RESULTS.md) |
| Same native kernel, different wrappers | [Shared kernel](compare/SHARED.md) |
| Capture versus live output | [Streaming](../docs/STREAMING-HISTORY.md) |
| Finite versus incremental input | [Input](../docs/INCREMENTAL-STDIN-HISTORY.md) |
| Framed worker adapters | [Duplex comparison](duplex_compare/RESULTS.md) |
| Wrapper profiles and native reload change | [Grouped wrapper](duplex_shared/RESULTS.md) |
| Resident output/input observations | [Output soak](STREAM-SOAK.md), [duplex checks](../docs/DUPLEX-VALIDATION.md) |
| Older Honk consumer | [Short soak](SOAK.md), [sustained run](SUSTAINED.md) |
| Older goose-check invocation overhead | [Checker consumer](../docs/CHECKER-CONSUMER.md#invocation-cost-experiment) |

See [the currentness audit](CURRENTNESS.md) for exact source scope. No current
cross-language speed, peak-memory or general reliability ranking is claimed.
