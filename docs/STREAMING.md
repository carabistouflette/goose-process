# Read output as it arrives

Use `Output.Stream` when you want to inspect output without keeping a whole log.
Start a Child, provide output buffers to `stream_step`, and consume the reported
prefixes before reusing those buffers. This API is on main, not v0.1.0.

After `make setup`, try the compiler example:

```sh
.toolchain/goose/build/goose run examples/compiler.goose
```

It sends valid and invalid C source to `/usr/bin/cc`, counts diagnostic bytes and
newlines as they arrive, then prints `streamed compiler diagnostics checked`.
It needs that host compiler. It is a fixed example, not a general command runner.

For each streamed route, `stream_step(child, out, err, timeout_ms)` returns
`(status, done, stdout_count, stderr_count)`. Only the indicated prefixes are
valid. Process them even on a terminal error. An empty buffer pauses that route;
it does not discard its data. A child can block if you never provide space.

One call handles bounded work. Continue until `done`, or call `close` to cancel.
A zero timeout polls without waiting; it does not disable the Child's overall
deadline. `Output.Capture` can be mixed with Stream when one stream should be
retained. Stream has no cumulative byte cap, so apply your own workload policy.

See [the full stream contract](API.md#bounded-live-output) for routing, buffer
lifetimes and EOF behavior, and [caller cleanup](CALLERS.md) for signals and
exceptional transferred children. Descendants retaining output descriptors can
keep streams open after the direct child exits.

[Tests and old measurements](STREAMING-HISTORY.md) describe their named snapshots.
[Current benchmark scope](../bench/CURRENTNESS.md) distinguishes them from main.
