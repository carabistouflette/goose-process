# Integrating an application with Child

These rules apply to applications that hold locks while running a command, such
as Honk. [API.md](API.md) defines the library contract. The snippets below are
control-flow examples; signal guards and transferred-child recovery belong to
the application and are not global library hooks.

## Signals, PID observations and cleanup

Use a local `Child` and explicit options. For a CLI preserving ordinary terminal
behavior, select `Input.Inherit`, independent stdout Capture/Inherit, and stderr
Inherit. Set capture bounds and a deliberate deadline rather than inheriting the
30-second default. A 24-hour deadline is the supported maximum; selecting it
still changes an application that previously had no finite deadline.

An application signal guard can save/normalize SIGCHLD and latch SIGINT/SIGTERM
metadata. Its handler must only record `sig_atomic_t` flags, not call Goose or
signal a cached numeric PID. Take and clear pending flags while the relevant
signals are blocked. Keep the guard and application locks until cleanup is
complete. Establish the guard before start so a pending signal cannot exit the
application in the launch/registration window.

```text
save dispositions/mask; normalize SIGCHLD to default, no SA_NOCLDWAIT
install application handlers that only latch signal + origin
start(args, explicit_options, child)
record any transferred PID from this result immediately
while an active Child remains:
    take pending signals under the application guard
    forward applicable signals through send_signal(child, signal)
    status, complete = step(child, 20)
    record any transferred PID from this result immediately
    if complete: leave the loop
close(child) and record any transfer
recover any transferred child before releasing locks/restoring SIGCHLD
return the application's latched cancellation status, or the child result
```

Do not externally wait/reap a library-owned child. `try_wait` may reap but does
not drain/feed; its completion flag alone is insufficient to release locks or
return complete captures. `step`/`wait` may reap before stream EOF if descendants
hold captures. After reaping, `id` returns -1 even while captures remain active.
`send_signal` then returns InvalidState without making a signaling syscall.
Repeated wait/close do not signal a reaped child.

A positive `id` is suitable for an immediate observational query before another
operation mutates the owner. With no external reaper, natural exit leaves its
zombie PID reserved. Do not cache the number across wait/step/try_wait or send
signals through it. Observational queries can return ESRCH and should not turn
that into an assumption about group membership.

For terminal Ctrl-C, the kernel normally signals every member of the foreground
group already. A caller may suppress forwarding a kernel-origin SIGINT when the
still-unreaped child shares its group. Forward a direct-PID SIGINT normally.
Use `send_signal`, never numeric kill, for library-owned child forwarding. Exact
duplicate suppression across a child concurrently changing groups is best effort;
this library does not provide job control or group supervision.

The maintained [lifecycle regressions](../tests/lifecycle.c) exercise positive
identity before reaping, hidden identity afterwards with undrained captures,
refusal to signal after reap, repeated waits, and owner reuse. Application tests
should additionally cover terminal/direct cancellation and lock retention around
startup, natural exit and exceptional cleanup.

## Exceptional transfer is a separate owner

A nonzero `Status.unreaped_pid` transfers responsibility **out of Child**. Latch
it from every operation, including failed start; the field is emitted only once.
The Child is closed after transfer. Repeating close/wait is not a recovery API,
and reusing that owner does not finish the transferred child.

```text
transferred_pid = first nonzero status.unreaped_pid
keep application locks, SIGCHLD default, and exclusive reaping ownership
until terminal wait status or ECHILD is established:
    retry interrupted waits
    signal only while the PID is still owned and unreaped, if permitted
    if signaling is denied, wait for natural termination
    if waiting is persistently denied, diagnose and retain the execution scope
once reaped/ECHILD: never signal that PID again
only now release locks and restore the signal guard
```

Numeric operations on this *transferred, exclusively owned and unreaped* PID
are distinct from caching a library-owned PID across a reaping operation. A
caller can obtain its own pidfd while the PID reservation is held. It must never
restore SIGCHLD=SIG_IGN/SA_NOCLDWAIT, introduce another reaper, or signal after a
terminal wait/ECHILD. A denial that prevents proving cleanup cannot be made safe
by returning normally and unlocking. An application may need to wait indefinitely
in this exceptional failure state; the library's normal deadline cannot override
OS permissions. External termination of the parent also releases its locks and
is outside this guarantee.

The lifecycle fault test denies both cancellation routes, checks exactly one
transfer, cleans up through a separate test owner, and verifies later Child
close/wait calls do not transfer or signal again. These fault tests cover the
library handoff; an application's recovery helper needs its own denial tests.

## Environment and status policy stay with the caller

Use per-child EnvOverride entries for Git/tar sanitization, not mutations of the
parent environment. Opt-in PATH lookup uses the effective child environment and
cwd. An absolute executable avoids that lookup policy. The 4,096 argument bound
includes argv[0]; each capture and finite stdin is bounded to 1 MiB.

Map operation errors separately from child statuses. A caller preserving shell
conventions can map LaunchFailed/ENOENT to127, other executable failures to126,
ordinary library/setup/limit errors to125, and actual signals to128+signal.
Choose precedence explicitly: a latched parent cancellation may win after cleanup
even if the child handled that signal and exited0. Do not mistake Error.None for
exit0; use exit_code/signal or success for completed command outcomes. Never
ignore unreaped_pid while applying a status mapping.
