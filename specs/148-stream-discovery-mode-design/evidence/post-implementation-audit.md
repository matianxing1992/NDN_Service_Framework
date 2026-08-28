# Post-Implementation Audit: Spec 148

**Date**: 2026-07-25  
**Verdict**: **BLOCK**

## Audit summary

| Principle | Verdict | Evidence |
|---|---|---|
| Intent fidelity | PASS | predictive `start/push/flush` is the sole public facade |
| Occam/necessity | PASS | no compatibility alias or runtime Mapping-first selector remains |
| Ownership | PASS | App signs source Data; Core retains and returns exact wire |
| C++/Python parity | PASS | replacement descriptor and lifecycle are bound symmetrically |
| Security/correctness | PASS | validator, expected provider, epoch, name, wire cap, duplicate/equivocation and stop fences are enforced |
| Workload neutrality | PASS | predictive Core contains no UAV, telemetry, audio, video, codec, or workload branch |
| Build/native evidence | PASS | full build and 394-test native suite pass |
| Python evidence | BLOCK | full suite: 1,091 run; 2 failures, 10 errors, 5 skips |
| Zero-loss MiniNDN | PASS | 76.042 measured seconds, 99.842% delivery, exact wire, 100% future hit, zero Mapping/retry/timeout/Nack |
| Impaired MiniNDN | BLOCK | 1.424% delivery, 39,488 Mapping Interests, 5,087 retries, 6,821 timeouts |
| Evidence integrity | PASS | two cells executed once; hashes stable; negative cell preserved |

## Code-aware finding

`PredictiveStreamSubscriber::beginRecovery()` starts recovery once per missing
cursor. `fetchRecoveryFrontier()` then expresses a frontier Interest for that
cursor, and `fetchRecoveryGroup()` walks
`frontier.retainedGroupCommitNames` backwards one group at a time until it
finds the source name. There is no shared frontier fetch, cursor-to-group
index, or coalesced recovery transaction across concurrent gaps.

This explains why FEC was observably active (2,318 repair attempts and 513
recoveries) but did not keep the impaired cell stable. The lookup/control
amplification is not a UAV special case; it is a generic predictive recovery
boundary.

## Gate decision

T001–T020 were executed. T021 and feature closure remain blocked. Spec 148 must
not be rerun or silently extended with a new recovery algorithm. A successor
Spec should define bounded, coalesced recovery discovery and validate it in a
fresh campaign against this immutable negative baseline.
