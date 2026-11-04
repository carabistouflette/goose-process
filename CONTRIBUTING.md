# Contributing

Keep the API small and explicit. Preserve argument boundaries, bounded work,
ownership, descriptor isolation, and child cleanup. Do not add implicit shells,
PATH searches, process-global signal handlers, or platform-specific application
policy. Linux/glibc is the initial platform; unsupported platforms must fail
clearly rather than silently weaken the contract.

Use focused branches (`feat/...`, `fix/...`, `docs/...`) and Conventional Commits:
`feat: add ...`, `fix: reject ...`, `docs: clarify ...`, `test: cover ...`,
`chore: update ...`. Subjects are imperative and each commit is one coherent
change. No stability guarantee exists before 1.0.

Behavior changes require public-API regression coverage and independent review.
Run `make format check test`, `make sanitize SANITIZER_CC=gcc`,
`make sanitize SANITIZER_CC=clang`, and `git diff --check`. Native source changes
must refresh the package fingerprint with `make format`. Never commit generated
compiler output, credentials, or private workspace notes. Compiler changes
require a reviewed full upstream revision in `GOOSE_REVISION` and full validation.

Profile before optimizing. Preserve safety checks, keep raw benchmark samples
with compiler, platform, flags and source revision, and avoid universal claims
from local measurements. CI checks execution rather than timing thresholds.

Report vulnerabilities privately through repository security advisories when
available. Use semantic versions only when a reviewed release is ready.
