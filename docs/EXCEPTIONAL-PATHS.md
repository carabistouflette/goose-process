# Exceptional-path coverage audit

Audit base: `e19080fc5995d5361d3a58d21d1006972ba6f84c`, 2026-11-04.
The audit compared [API contracts](API.md) and [caller ownership rules](CALLERS.md)
with the maintained native and public tests. Two independent auditors examined
launch/resource and lifecycle/cancellation paths. No production defect was
confirmed. The changes add regression coverage; runtime code and release tags
are unchanged.

## Existing coverage retained

| Contract or failure class | Maintained evidence |
| --- | --- |
| Descriptor exhaustion and partial FD cleanup | `exhausted_descriptors` in [native.c](../tests/native.c) lowers real RLIMIT_NOFILE through 3–15, restores it and checks FD recovery. The syscall matrix separately injects EMFILE in capture pipe/low-FD promotion paths. |
| Argument allocation, null-device open, pipe/flag setup and spawn-action errors | [syscalls.c](../tests/syscalls.c) injects failures across all eight standard-FD configurations, checks prefixes/canaries, exact FD recovery, no transferred owner, and no child/zombie. |
| Spawn return codes and primary-error preservation | The syscall matrix poisons errno to distinguish APIs returning error codes directly; close-error simulation does not replace the primary error. |
| Missing executable/cwd, malformed executable, PATH permission/lookup semantics, actual exit 126/127 | [native.c](../tests/native.c), [extended.c](../tests/extended.c), [model.py](../tests/model.py), and the public [API tests](../tests/api.goose). ENOEXEC does not invoke a shell. |
| Finite stdin setup/send failures and concurrent output | `input_and_faults` in [extended.c](../tests/extended.c) covers socket/shutdown/send failures and duplex pressure. |
| pidfd-open failure, cancellation fallback, denied cancellation/wait and stopped statuses | [failures.c](../tests/failures.c) exercises fallback or explicit transfer while preserving child ownership. |
| Exactly one transfer, no signal after reap, retained descendant-held captures and owner reuse | [lifecycle.c](../tests/lifecycle.c) checks ownership independently of stream completion. |
| Absolute deadlines and slice bounds under interrupted waits | [deadlines.c](../tests/deadlines.c), interrupted-step lifecycle tests, and the real timer-interruption test in native.c. |
| Application startup handoff, forwarding, retained locks and exceptional recovery | [caller.c](../tests/caller.c) uses scoped handlers, readiness gates and selected denial faults. Honk has its separate application tests. |

## Coverage gaps closed

- `recoverable_operations` in [failures.c](../tests/failures.c) denies a
  nonblocking observation once and injects EINTR followed by EPERM during direct
  signal forwarding. The error leaves the same owner active with no transfer.
  Later observation/signaling/wait succeeds. Direct forwarding never falls back
  to numeric kill; terminal cleanup remains a separate operation.
- `failed_start_transfer` combines failed pidfd opening with denied numeric
  cancellation. The failed start transfers ownership once, closes the Child,
  and repeated close does not transfer again. A separate test owner recovers the
  unreaped child before the library owner is successfully reused.
- `denied_after_exit` establishes terminal exit with WNOWAIT while retaining the
  PID reservation, then denies both cancellation routes. Cleanup still reaps,
  preserves actual exit 0, reports the denial, and emits no transfer. The fixture
  uses an explicit exit observation rather than a timing-based sleep.
- The separate SA_NOCLDWAIT case in [native.c](../tests/native.c) checks the
  auto-reaping precondition with SIGCHLD at its default handler. The pre-existing
  SIG_IGN and child signal-reset checks remain intact.
- `preparation_faults` in [syscalls.c](../tests/syscalls.c) denies the second
  allocation while building an explicit environment, after argument allocation
  succeeded. It also denies each of three borrowed-FD snapshot duplications.
  Each case verifies the injection occurred before any spawn, returns the
  expected error, leaves a reusable closed owner, retains original descriptors
  and both descriptor/status flags, restores the FD count and leaves no child.
  Sanitizers check allocation cleanup.
- `kernel_argument_budget` lowers RLIMIT_STACK only in a separate fixture
  process and passes two valid 100,000-byte arguments. On 4 KiB-page Linux this
  exceeds the reduced kernel exec budget while remaining below library limits:
  LaunchFailed/E2BIG, no exit/transfer, restored descriptors and no child.
  Non-4-KiB-page hosts explicitly skip this fixture because the kernel's minimum
  argument budget depends on page size. The parent stack limit is unchanged.

Two independent reviewers checked the new regressions. Both caught a shared
signal-fixture mutation that would have weakened the old ignored-SIGTERM test;
it was corrected with separate signal-action structures before final validation.

## Reproduction and limits

```sh
make format check test
make sanitize SANITIZER_CC=gcc CFLAGS='-std=c11 -Wall -Wextra -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3'
make sanitize SANITIZER_CC=clang CFLAGS='-std=c11 -Wall -Wextra -Werror -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=3'
make fuzz
git diff --check
```

The new fault-injection cases target the private native ABI; normal public Goose
integration and ownership checks run in the full test target. Permission, memory
and syscall denials are deterministic linker-wrapper simulations, not actual
credential changes, system-wide memory exhaustion or process-limit exhaustion.
Actual resource-limit fixtures cover RLIMIT_NOFILE and the conditional
RLIMIT_STACK/E2BIG case. No RLIMIT_AS, RLIMIT_NPROC, exhaustive errno matrix,
external-reaper compatibility or thread-cancellation guarantee is claimed.
Caller-host preconditions remain required. Goose allocator failure follows the
Goose runtime policy; native ENOMEM checks do not redefine it. This is a bounded
coverage audit, not a formal proof or an absence-of-bugs claim.

Recorded local validation on 2026-11-04 passed all commands above, including the
full normal suite, both fortified ASan/UBSan suites and 10,000 intercepted fuzz
inputs. The E2BIG fixture ran on this 4 KiB-page host. Runtime/native source was
checked unchanged against the audit base. Hosted CI is a separate validation
step; local completion does not imply a hosted run has passed.
