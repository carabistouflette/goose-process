# Historical subprocess comparison

This dated research describes b97176d, not current main. API capabilities and
external tool versions below belong to that snapshot. See [current scope](COMPARISON.md).

## Historical baseline assessment

Research date: **2026-11-04**. This is a source-based capability assessment, not a benchmark result or a promise of implementation. Goose does **not** currently provide universal parity with Rust, Go, or Python. No speedup has been established here.

## Evidence and versions

The Goose baseline is public main **b97176d8877044af32356f59516403abf0141a7d**. The inspected files are [process.goose][GP], [native/process.c][GC], [docs/API.md][GA], and [README.md][GR]. All “current Goose” entries below refer to that snapshot, not future work. It is an experimental, unreleased Linux/glibc library. [GR]

Official comparator pages retrieved on the research date identify **Rust std 1.99.1** (build `58cf8f9c3`, 2026-10-13), **Go go1.27.3**, and **Python documentation 3.15.2**. Rust stable and Python `/3/` URLs move; the linked tagged source preserves implementation evidence. These are the versions observed in the references, not a claim that every floating page is synchronized or that these toolchains were installed or benchmarked. Go's release history dates go1.27.3 to 2026-10-19. [RC] [GE] [GV] [PS] [RS] [PY]

Only project-owned documentation and source are used. API absence means absence in the named interface, not impossibility using syscalls, another library, a shell, or application code. Rust nightly APIs are not counted as stable features.

## Shared synchronous Linux command execution

Cells link directly to evidence. “Proposed” is a recommendation only. “Unsupported” describes the current Goose API. Baseline support may require explicit configuration or composition; equivalent defaults are not implied.

| Capability | Current Goose | Rust `std::process` | Go `os/exec` | Python `subprocess` | Goose recommendation |
| --- | --- | --- | --- | --- | --- |
| Execute argv and wait | `run`, absolute executable; no implicit shell [GP] [GC] | `Command::status/output` [RC] | `Cmd.Run/Output` [GE] | `run`, default `shell=False` [PS] | Current; preserve literal arguments |
| Working directory | Optional `cwd` [GA] | `current_dir` [RC] | `Dir` [GE] | `cwd` [PS] | Current |
| Environment selection | Inherit only [GC] | `env/envs/env_remove/env_clear` [RC] | `Env`, `Environ` [GE] | `env` replacement mapping [PS] | Proposed inherit, replace, override/remove |
| Executable lookup | No PATH or relative executable [GP] | PATH lookup [RC] | `LookPath`, `Command` [GL] [GE] | POSIX execvpe-like lookup [PS] | Proposed explicit opt-in lookup |
| Stdin | Always null device [GC] | Inherit/null/pipe/file [RI] | Reader/file/pipe; nil means null [GE] | Inherit/null/pipe/file, `input` [PS] | Proposed bounded bytes, inherit, file |
| Stdout/stderr independently | Both captured separately, or both inherited [GP] | Independent `Stdio` [RI] | Independent writers/files/pipes [GE] | Independent destinations [PS] | Proposed per-stream modes |
| Discard/file output | Unsupported [GP] | Null/file [RI] | Nil output means null; file writer [GE] | `DEVNULL`/file [PS] | Proposed null/file |
| Merge stderr into stdout | Unsupported [GP] | Share/duplicate destination via file/pipe composition [RI] | `CombinedOutput` [GE] | `stderr=STDOUT` [PS] | Proposed explicit merge |
| Binary capture | Separate byte buffers [GA] | `Output` byte vectors [RC] | `Output` bytes [GE] | Default bytes; optional text conversion [PS] | Current; keep bytes as core |
| Nonzero exit vs launch failure | Separate operation error, exit code, signal [GA] | I/O error vs exit status [RC] | Launch error vs `ExitError` [GE] | Launch exception vs returncode/check exception [PS] | Current; preserve distinction |
| Capture size policy | Per-stream hard limit; overflow cancels [GA] | No capture cap option in `output` [RC] | No cap option in `Output`; custom writers possible [GE] | No cap option in `run/communicate` [PS] | Current; retain bounded default |
| Deadline | Required positive timeout; kill/reap on expiry [GA] | No timeout parameter on stable wait/output [RH] | `CommandContext`; configurable `Cancel/WaitDelay` [GE] | `run(timeout=...)` [PS] | Current, but semantics differ |

The shared synchronous target should include stdin, environment control, independent stdio, working directory, binary output, and accurate outcomes. PATH lookup is common convenience but should remain opt-in. A bounded runner can cover these workflows without reproducing every comparator method. Fixed size limits and mandatory finite deadlines remain deliberate departures from unrestricted baseline APIs. [GP] [GA] [RC] [GE] [PS]

## Handles and other distinct scopes

| Scope | Current Goose | Comparator evidence | Recommendation, not current support |
| --- | --- | --- | --- |
| Child handle and lifecycle | Only synchronous `run`; exceptional `unreaped_pid` is ownership transfer, not a normal handle [GA] | Rust `Child`: id, wait, try_wait, kill, pipe handles [RH]. Go Start/Wait with `os.Process` [GE] [GO]. Python `Popen`: poll/wait/send_signal/terminate/kill [PS] | Separate design milestone with explicit ownership and reaping |
| Interactive streaming | No caller-visible pipes or input [GP] | Rust pipe handles [RH] [RI]; Go pipe methods [GE]; Python `Popen` streams/communicate [PS] | Separate from finite-input synchronous extension |
| Pipelines | No pipe wiring [GP] | Rust child output converts to `Stdio` [RI]; Go pipes/files compose [GE]; Python documents connecting `Popen` instances [PS] | Add only with partial-launch cleanup and every-stage status |
| Groups and sessions | No group/session option; descendants not supervised [GA] | Rust stable `process_group`; `setsid` is nightly [RU]. Go Linux `Setpgid/Setsid` [GS]. Python `process_group/start_new_session` [PS] | Typed Linux policy; group cancellation does not contain escaping descendants |
| Credentials and privilege setup | No uid/gid/groups/umask options [GP] | Rust stable uid/gid, nightly supplementary `groups` [RU]. Go Linux `Credential`, namespaces/capabilities/chroot controls [GS]. Python user/group/extra_groups/umask [PS] [PC] | Explicit separate security scope; never imply sandboxing |
| Descriptor passing | Close everything >=3; no allowlist [GC] | Go `ExtraFiles` (not Windows) [GE]; Python POSIX `pass_fds` [PC]; Rust `Stdio` accepts owned descriptors for stdio [RI] | Keep isolation; add allowlist only if required |
| PTY | Unsupported [GP] | Python has a separate Unix `pty` module [PT]. Go Linux exposes controlling-terminal attributes, not a PTY allocator in `os/exec` [GS]. Rust `Stdio` accepts files but has no PTY constructor [RI] | Separate terminal/session module; pipes do not supply terminal semantics |
| Other platforms | Linux kernel >=5.3, glibc >=2.34 [GR] | Rust has Windows extensions [RW]; Go documents Windows lookup/argv differences [GE]; Python has Windows creation/startup controls [PS] | Unsupported until implemented and tested per platform |
| Shell/text convenience | No implicit parsing or decoding [GA] | Rust/Go literal argv interfaces [RC] [GE]; Python offers explicit shell and text modes [PS] | Keep core byte-oriented; explicit shell execution remains possible as a command |
| Replace current process / pre-exec hooks | Unsupported [GP] | Rust Unix `exec` and unsafe `pre_exec` [RU]; Python `preexec_fn` [PC] | Out of initial scope; avoid arbitrary post-fork callbacks |

Python also exposes buffering, encoding/error policy, Linux pipe-size requests, auditing, and Windows startup flags; those are additional API surface, not prerequisites for a small Linux synchronous runner. [PY] [PS] Full Python API equivalence would require a substantially broader scope than the shared target.

## Semantic differences that affect correctness

### PATH, environment, cwd, and argv

Go resolves a bare command through the **parent** PATH at `Command` construction; assigning `Cmd.Env` later does not redo that search. Its Unix lookup rejects a result relative to the current directory with `ErrDot`. A relative `Cmd.Path` is evaluated relative to `Dir`. [GL] [GE]

Rust's current Unix documentation says lookup uses the child's PATH; removing PATH without replacement permits an OS-defined default. It warns that combining a relative executable with `current_dir` is platform-dependent. [RC] Python POSIX lookup uses the supplied environment and cwd; Windows differs. [PS] Goose avoids this ambiguity by requiring an absolute executable and passing inherited `environ`; changing cwd does not rewrite that path or explicitly update `PWD`. [GP] [GC]

Proposed contract: retain absolute execution as default; expose search as a separate explicit policy with documented PATH source, empty/relative components, unset PATH behavior, cwd timing, and failure classification. Define environment inheritance/replacement distinctly, including duplicate keys, invalid keys/NUL, empty values, removal, byte limits, and no mutation of parent state. Do not silently adopt one baseline's lookup semantics and advertise all three.

Goose bounds are 256 arguments including argv[0], 65,536 packed bytes including terminators, 16,384 bytes per argument, and 4,095 bytes for executable/cwd. argv[0] also selects the executable; no independent override exists. [GP] Rust Unix `arg0` and Python `executable` separate these concepts; Go exposes Path and Args separately. [RU] [PY] [GE] The comparator interfaces do not promise these Goose-specific bounds; successful validation is not a guarantee the OS will accept a launch. [RC] [GE] [PY] Preserve OS launch errors, including argument/environment size failures, rather than reporting every such failure as Goose validation.

### I/O, memory, and progress

Goose accepts exactly the configured limit per stream; an additional observed byte cancels. Zero accepts only empty output. Partial captures survive cancellation, without a cross-stream ordering guarantee. Capture buffers are cleared first; the wrapper initializes each to its configured capacity before calling C. Limits are 1 MiB per stream, default 64 KiB. [GA] [GP]

Rust `spawn/status` inherit stdio by default, while `output` captures output and gives stdin immediate EOF. Go's nil stdio is null, not inheritance. Python's default stdio is inherited. [RC] [GE] [PS] Benchmarks must configure these explicitly.

Proposed finite stdin must write while concurrently draining both outputs; close stdin at input exhaustion, handle early child closure without killing the parent via SIGPIPE, and retain deadline checks under sustained I/O. A merged stream needs one actual shared destination and a specified cap; concatenating separate captures after exit is not equivalent. A handle API must define backpressure and who drains, closes, cancels, and reaps. Rust's docs warn of full-pipe deadlock and do not automatically kill/wait when a Child is dropped. [RI] [RH]

### Signals, waits, deadlines, and descendants

Goose resets child signal dispositions to defaults and clears its mask. It polls pipes and a pidfd, kills only its direct child on cancellation, and waits for terminal status. Its deadline begins in native preparation **after** argument copying, and excludes earlier Goose validation/packing/allocation. It is not a hard end-to-end wall-clock bound. A descendant holding a capture pipe can cause timeout after the direct child has exited. Failed cancellation/reaping may transfer ownership through `unreaped_pid`. [GC] [GA]

Python `run` kills and waits on timeout; `Popen.communicate(timeout)` does not itself kill, so cleanup is the caller's job. Python 3.15 documents event-driven timed waits using Linux pidfds when supported: an assertion that current Python always busy-polls is false. [PS] [PY] Go's default context cancellation kills the direct Process; WaitDelay additionally limits certain child/pipe waits, but arbitrary blocking Readers/Writers can still delay completion. [GE] [GX] Rust stable `Child` has wait/try_wait/kill but no timed wait parameter; kill still requires reaping. Unix signal details are exposed through ExitStatusExt. [RH] [RE]

Proposed group cancellation needs an explicit group-creation policy, graceful-signal/escalation rules if offered, and safe ownership through races. A process group is not reliable whole-tree containment: descendants can change session/group. Do not claim group setup alone supervises every descendant. Existing Goose explicitly excludes descendant supervision. [GA]

### Descriptor and security policy

Goose explicitly closes all descriptors >=3, including foreign descriptors without CLOEXEC. [GC] Python defaults to `close_fds=True`, with POSIX exceptions through `pass_fds`. Its C implementation explicitly closes descriptors except its keep list. [PS] [PC] Rust and Go do not expose an equivalent blanket `close_fds` switch in the compared process builders; do not infer closure of arbitrary foreign inheritable descriptors from safe defaults for their own files. Audit and test actual host descriptor inheritance. [RC] [RS] [GE] [GS]

Python restores selected signals ignored by Python, rather than promising Goose's reset-all policy. Its pre-exec callback is documented unsafe with threads; Rust's corresponding callback is unsafe and constrained to async-signal-safe work. [PC] [RU] Preserve Goose's no-parent-global-mutation approach when extending functionality. Literal argv avoids implicit shell interpretation, but the invoked program still interprets options/scripts, and inherited environment can carry credentials; current Goose is not a sandbox. [GA]

## Proposed implementation order and acceptance gates

These are recommendations, not delivered features or evidence that tests already pass.

1. **Freeze existing guarantees.** Keep binary argv/output, separate launch/exit failures, bounded draining, descriptor closure, and explicit exceptional child ownership. Run existing validation, integration, sanitizers and fault injection before and after each extension; the current README describes these checks. [GR]
2. **Extend synchronous options.** Add typed environment policy, finite byte stdin, and independent inherit/null/capture/file stdio. Avoid a second overlapping set of capture booleans. Retain finite defaults; publish any incompatible API transition.
3. **Add opt-in lookup and group policy.** Resolve/document search semantics first. Keep process-tree containment out of the claim unless independently implemented.
4. **Decide broader parity explicitly.** Handles/streaming/pipelines, PTYs, credentials and non-Linux support need separate contracts and review. Until then, list them as unsupported, not “equivalent with a wrapper.”

Concrete acceptance cases:

| Area | Required checks and oracle |
| --- | --- |
| argv/cwd/lookup | Child reports length-delimited argument bytes and actual cwd. Cover empty arguments, invalid UTF-8, whitespace/metacharacters, NUL rejection, every current boundary +/-1, absent/denied/malformed executable, invalid cwd, and launch failure versus real exits 126/127. Proposed lookup: two competing binaries across parent/child PATH and cwd, unset/empty PATH/components, relative names, permissions and no implicit shell fallback. |
| Environment | Child reports exact selected keys: inheritance, fully empty replacement, overrides/removal/empty values, duplicates and invalid keys/NUL. Confirm parent unchanged and concurrent calls isolated; test combined argv/environment OS refusal. |
| Stdio/input | Exercise every valid independent mode combination; binary NUL/invalid UTF-8 payloads, empty input and input larger than pipe capacity. Child emits both streams before reading, reads slowly, exits without consuming input, or closes stdin early. Require no deadlock, correct bytes, expected EOF and stable parent SIGPIPE state. |
| Capture/merge | For each stream independently: 0, limit-1, limit, limit+1; saturate both simultaneously, including after stdin stalls. Check retained prefixes and status, no assumed ordering across separate streams. For merge, validate the child's serialized shared-stream writes and agreed cap. |
| Deadlines/ownership | Child sleeps, ignores graceful signals, stops/is traced, exits at cancellation, or exits while descendant holds pipes. Inject signal/reap errors and EINTR; verify no unrelated PID is signaled and any transferred child is explicitly cleaned up by test owner. Measure overshoot without treating scheduling noise as deterministic failure. |
| Groups/credentials, if added | Descendant remains in group versus escapes with setsid; verify advertised cancellation scope. Under controlled permissions verify uid/gid/supplementary groups/umask and failures; never silently continue with parent credentials. |
| Descriptor safety | Child enumerates inherited fds; include high-number non-CLOEXEC fds, stdio initially closed, explicit future allowlist, concurrent spawn/open pressure and exhausted fd limit. Confirm parent fds unchanged and no descriptor/zombie growth over repeated calls. |
| Handles/pipelines, if added | Poll/wait repetition contract, abandoned handle ownership, concurrent cancellation, early consumer exit, producer saturation, stage-two launch failure, status of every stage, and cleanup of all started children and pipe ends. |

Have independent reviewers check API/semantics and native lifecycle/security separately, then independently re-check the final fixes. Preserve the evidence and unresolved objections. Use small Conventional Commits for subsequent implementation (for example `feat(process): add finite stdin input` and `fix(process): reap child after cancellation race`); this research makes no commits.

## Comparable end-to-end benchmark proposal

No timing measurements were made for this document. Benchmark the complete public call through status validation and available output, not just `posix_spawn` or the C ABI. Use the same compiled child fixture and payloads for every language; keep shell/interpreter startup out of the child unless it is the workload being measured.

| Workload | Definition and validation |
| --- | --- |
| Empty child | Absolute fixture exits 0 with null stdin and explicitly configured stdio. Separate capture and inherit cases; check status and zero bytes. |
| Small/large captures | Fixed binary payloads: 64 B, 4 KiB, 64 KiB, then 1 MiB per stream; separate stdout-only and simultaneous stdout/stderr cases. Validate length and hash outside timed region. Set an equal cap large enough for success. |
| Arguments and cwd | Length-delimited echo of 1, 32, 256 arguments within Goose limits, with short and near-limit aggregate bytes; explicit cwd. Compare exact bytes/cwd. |
| Finite stdin, once supported | Echo/hash input of 0 B, 4 KiB and 1 MiB while producing stderr concurrently. Validate input hash and both outputs. Current Goose is ineligible, not “faster.” |
| File/null modes, once supported | Same bytes written to null or the same filesystem/file policy. Distinguish buffered writes from fsync durability. Validate output files and size separately. |
| Launch failure / nonzero exit | Missing executable, denied executable, invalid cwd, exit 7 and signal termination. Validate normalized result without conflating error construction with execution. |
| Cancellation / overflow | Quiet sleeper and unlimited dual-stream producer, equal deadlines/caps and direct-child cleanup policy. Report return latency, overshoot, retained bytes and cleanup outcome separately from successful throughput. |
| Descendant-held pipe | Direct child exits while fixture descendant retains output; identical pipe-close/deadline policy and an external cleanup owner. Validate no leaked fixture processes. |

Two result tracks are useful:

- **Native API track:** idiomatic APIs with explicitly listed semantics. Rust output has no built-in timeout/cap; Python capture accumulates in memory; Go capture helpers have no hard cap. These runs are descriptive and cannot establish equal safety-cost performance. [RC] [GE] [PS]
- **Matched contract track:** adapters enforce the same absolute executable, environment, stdin, stdio, per-stream byte caps, deadline origin, overflow cancellation, descriptor policy, and direct-child reaping. Include adapter work in timing. Rust may require pipe readers and a deadline controller; Go needs bounded writers/pipe control and explicit pipe cleanup; Python needs bounded incremental reads rather than unbounded communicate. Validate the adapters independently. [RI] [RH] [GE] [PY]

Match reset-signal behavior and descriptor guarantees, or explicitly mark the unmatched security overhead. An adapter that races with host fd creation to close foreign descriptors is not equivalent isolation. Do not weaken Goose defaults to win a timing comparison. Do not compare Goose's truncated prefix to complete unbounded output as equal work. Use successful finite-output runs to compare raw end-to-end cost, and a separate matched-limit stress suite to compare safety behavior.

Compile release builds with pinned toolchains; record Goose/compiler/native flags, Rust profile, Go version/settings, Python interpreter build, architecture, kernel, glibc, CPU, environment size, open fd count, affinity, governor and host load. Include validation/packing/allocation and zero-fill costs. Measure warm long-lived drivers separately from cold driver startup/import/compilation; compilation is never silently charged to one language alone. Report reusable-buffer and fresh-buffer cases separately where applicable.

Predeclare samples and warmups; discard the same warmup phase for each implementation. Randomize or rotate interleaved language blocks, repeat across independent sessions, and retain raw samples and failures. Report median, tail latency with enough samples, dispersion/confidence intervals and end-to-end operations/second; measure CPU and peak memory in separate instrumented runs. Label ratios by exact workload, machine and contract. Do not omit failed/timeout samples from success-rate reporting or turn a single microbenchmark into a universal speed claim.

## Official references

[GP]: https://github.com/carabistouflette/goose-process/blob/b97176d8877044af32356f59516403abf0141a7d/process.goose
[GC]: https://github.com/carabistouflette/goose-process/blob/b97176d8877044af32356f59516403abf0141a7d/native/process.c
[GA]: https://github.com/carabistouflette/goose-process/blob/b97176d8877044af32356f59516403abf0141a7d/docs/API.md
[GR]: https://github.com/carabistouflette/goose-process/blob/b97176d8877044af32356f59516403abf0141a7d/README.md
[RC]: https://doc.rust-lang.org/stable/std/process/struct.Command.html
[RH]: https://doc.rust-lang.org/stable/std/process/struct.Child.html
[RI]: https://doc.rust-lang.org/stable/std/process/struct.Stdio.html
[RU]: https://doc.rust-lang.org/stable/std/os/unix/process/trait.CommandExt.html
[RE]: https://doc.rust-lang.org/stable/std/os/unix/process/trait.ExitStatusExt.html
[RW]: https://doc.rust-lang.org/stable/std/os/windows/process/trait.CommandExt.html
[RS]: https://github.com/rust-lang/rust/blob/1.99.1/library/std/src/sys/process/unix/unix.rs
[GE]: https://pkg.go.dev/os/exec@go1.27.3
[GX]: https://go.dev/src/os/exec/exec.go
[GL]: https://github.com/golang/go/blob/go1.27.3/src/os/exec/lp_unix.go
[GS]: https://go.dev/src/syscall/exec_linux.go
[GO]: https://pkg.go.dev/os@go1.27.3#Process
[GV]: https://go.dev/doc/devel/release#go1.27.0
[PS]: https://docs.python.org/3/library/subprocess.html
[PY]: https://github.com/python/cpython/blob/v3.15.2/Lib/subprocess.py
[PC]: https://github.com/python/cpython/blob/v3.15.2/Modules/_posixsubprocess.c
[PT]: https://docs.python.org/3/library/pty.html
[OS]: https://docs.python.org/3/library/os.html#os.execvpe
