# Duplex wrapper cost and one native-state reload

## Validation scope

This work follows the checked duplex evidence at
`4c8fb3bbdc5789d2b3b9520d661b9ddc08bc328b`. The question is whether wrapper
work or differing process policies account for part of the observed session
cost. A shared-engine diagnostic cannot prove either implementation correct:
both front ends share native bugs as well as native policies.

Acceptance criteria:

1. Compare the unchanged public Goose framed driver with a small Rust front end
   calling the same native start/io_step/EOF/close implementation. Verify exact
   native object identities; do not substitute Rust std::process for this track.
2. Keep the worker, 128 sequential requests, 31/4096-byte payloads, binary frame/
   acknowledgment oracles, output spans, 30-second budget and explicit EOF/drain/
   reap unchanged. Account for staging/validation and document remaining wrapper
   and language-ownership differences. The native ABI is private.
3. Commit sources before recording shuffled matched samples. Retain every
   warmup/sample/status and source/compiler/object/executable identity, including
   failed attempts. Separate instrumented profiles from untraced timing.
4. Optimize only demonstrated work. The narrow candidate is gp_io_step's second
   load of the 192-byte child state solely to compute local input openness.
   Derive that result from the already validated local state instead; preserve
   the InvalidInput/InvalidState guards, all span/alias/state validation, accepted
   counts, metadata ordering, deadlines, pidfd identity and exceptional transfer.
5. Preserve public/native ABI and legacy operations. Cover outer alias rejection
   without mutation; inner rejection with zero progress and unchanged owner;
   open empty/EAGAIN offers; closed EOF/peer/reaped input; terminal success/error/
   transfer accounting. Run full normal GCC/Clang, both sanitizers, intercepted-
   spawn fuzzing and static analysis for any native change.
6. Independently review correctness/security and results. Rerun the four-adapter
   comparison and resident duplex soak. Retain raw timing/resource observations
   and their limits; no general performance, memory, security or stability claim.

## Design evidence

The preceding final-span profile attributes 37.11% of whole-driver instructions
to wait_impl and 10.31% to gp_io_step. A read-only independent design review
confirmed that the baseline GCC -O2 executable actually copies all 192 child
bytes after wait_impl, rather than the compiler eliminating the unused fields.
The proposed change removes that duplicate work; it does not remove validation
or clock observations, which enforce distinct failure and deadline contracts.

The equivalence argument is local: validation loads owner/phase; pumping keeps
the owner and leaves active or completed phase; emit changes only transfer
bookkeeping; storing that local value cannot invalidate its owner/phase. Buffers
cannot alias storage after validation. Concurrent mutation of one Child is not
a supported contract. Legacy callers need no new public result or state layout.

## Results

No new measurement or optimization result is claimed by this acceptance document.
Completed results will name their exact source revisions and limits.
