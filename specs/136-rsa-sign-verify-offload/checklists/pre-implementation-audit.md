# Spec 136 R3 Pre-Implementation Audit

**Date**: 2026-07-23  
**Scope**: R3 controlling documents, current NDN-SVS/benchmark implementation,
and retained R2 smoke evidence  
**Verdict**: **CONDITIONAL PASS**

R3 corrects the physically inadmissible R2 matrix. Implementation may proceed
only inside T004b. Formal execution remains blocked until the batching,
signer-service, drain, tamper, and no-op-pacer gates pass.

## Findings

| ID | Severity | Dimension | Finding | Required action |
|---|---|---|---|---|
| R3-001 | MEDIUM | Experiment validity | The R2 800/1000 cells demanded approximately 127.7%/159.6% of one serialized RSA signer before other Face work. Attempted-rate admission mislabeled overload as an admitted workload. | Keep those cells as `OVERLOAD_INVALID`; implement FR-019/FR-020 before sealing. |
| R3-002 | RESOLVED | Code/document agreement | The runner, benchmark, and analyzer now implement the R3 matrix, 5 ms batching, split signer timing, utilization calculation, and drain admission. | Preserve the candidate hashes and do not modify them after formal sealing. |
| R3-003 | MEDIUM | Backpressure boundary | The worker releases its preparation-queue slot before posting the prepared result to Face. At R2 800 pps, each peer had only 69-73 worker-pending jobs but 2239-2285 outstanding jobs at measure end. | Do not claim the existing queue bounds end-to-end work. FR-020 must reject any rate that fails complete drain. Core backpressure redesign is outside this minimal proof. |
| R3-004 | MEDIUM | Four-core resource validity | Fresh 600-pps cells pass signer-only utilization but randomly starve one peer's Face/NFD progress. A 16-commit fairness probe did not fix the result and was removed. | Treat 600 pps as `RESOURCE_INVALID`; keep formal rates at 200-400. |
| R3-005 | RESOLVED | Traceability | The former R0 `traceability.md` did not cover the R3 requirements. | Replace it with a supplementary R3 coverage map; controlling authority remains with spec/plan/tasks/contracts/audit. |
| R3-006 | RESOLVED | Receive-path isolation | Repeated Mapping announcements registered one subscription repeatedly, and the default publication Fetch window of 10 became an unrelated receive bottleneck. | De-duplicate by subscription ID, test worker/Face ownership directly, and freeze the common Fetch window at 64. |
| R3-007 | RESOLVED | Claim boundary | The first confirmation analyzer emitted a formal-sounding capacity verdict from one fixed-order non-formal pair and compared delivery p99 across 57.86% versus 100% survivor sets. | Use a distinct descriptive verdict, preserve terminal provenance, expose duplicates/heartbeat skips, and make delivery p99 conditional when completion differs. |

No CRITICAL or HIGH finding remains after replacing the unexecuted formal
matrix with 200/250/300/350/400 pps per peer and retaining 1000 pps as a no-op
pacer check. The retained 600-pps probe is `RESOURCE_INVALID` on the four-core
host.

## Code-Reality Evidence

- The benchmark serializes Data and Interest signing through one
  `TimedRsaSigner` mutex.
- Corrected 400 pps evidence measures mean Data sign at 0.5267 ms and mean
  Interest sign at 0.5427 ms. Two Data signatures plus one unbatched Sync
  Interest require approximately 1.596 ms per publication, or 626.6 pps before
  other Face work.
- `commitPreparedPublication()` inserts Data and Mapping before
  `updateSeqNo()`, so the R2 failure is not a pre-store advertisement bug.
- `SVSyncCore::setSyncInterestBatching(true, 5_ms)` already exists and is the
  minimal shared primitive; no new Sync algorithm is required.
- Worker preparation releases its capacity slot before the prepared callback
  reaches Face, so `accepted` and worker-pending alone do not establish
  sustainable processing.
- The repeated-Mapping regression fails `5 != 1` before de-duplication and
  passes afterward. The 128-item worker test proves off-Face preparation,
  on-Face ordered commit, and zero outstanding work.
- Fresh 10/60/10 confirmation at 400 pps/peer records 57.86% delivery in
  Face-inline mode and 100% in worker mode with zero worker Fetch timeout.
- Claim audit confirms the capacity counts and thread mechanism but limits the
  result to non-formal descriptive evidence. Delivery p99 is not an independent
  contrast at unequal completion, and the original inline terminal status must
  remain visible.

## Traceability

| Requirement group | Task/evidence owner |
|---|---|
| FR-001 through FR-013 | T003 implementation and T004 candidate/admission |
| FR-014, FR-017, FR-018 | T004b caller path, no-op pacer, and formal guard |
| FR-019 | T004b split signer timing and utilization calculation |
| FR-020/FR-021 | T004b drain/conservation, unique delivery, and load outcome admission |
| FR-015 | T004 analyzer and T006 report |
| FR-016 | T004 protection check, T005 isolated output, T006 freeze |
| SC-001 through SC-003, SC-007, SC-008 | T003/T004b preflight evidence |
| SC-004 through SC-006 | T004 manifest, T005 receipts, T006 contrasts |

## Readiness Scorecard

| Dimension | Ready? | Notes |
|---|---|---|
| Intent and scope | Yes | Still tests one worker versus Face-inline preparation. |
| Necessity | Yes | Batching and signer/drain gates directly prevent the observed false admission. |
| Architecture | Conditional | Existing batching is reused; core end-to-end backpressure remains outside scope and observable. |
| Security | Conditional | Tampered Data/Interest preflight remains open. |
| Task executability | Yes | T004b owns one cohesive candidate-admission outcome. |
| Evidence integrity | Yes | R2 800/1000 retained and downgraded to `OVERLOAD_INVALID`. |
| Formal readiness | No | Corrected R3 runner/benchmark/analyzer exist; tamper and no-op pacer gates remain open. |

## Metrics

- User stories: 4
- Functional requirements: 21
- Success criteria: 8
- Tasks: 6 total; T001 superseded, T002-T003 complete, T004-T006 open
- Findings: 0 Critical, 0 High, 3 Medium, 0 Low, 4 Resolved
- Structural audit: PASS

## Tool And Gate Record

- Context: repository instructions, constitution, active Spec 136, contracts,
  state, and retained smoke evidence loaded.
- CodeGraph: signer, worker preparation, commit/store/advertise, batching, and
  Fetch paths inspected.
- Spec Kit: R3 strict structure passes with 20 FR, 8 SC, 4 stories, and 6
  cohesive tasks.
- GSD: existing `.planning/STATE.md` remains the resumable continuation.
- ARS experiment discipline: impossible-load cells are excluded rather than
  tuned or selectively replaced.

## Next Action

Complete only the remaining T004b tampered-object and no-op-pacer gates, then
seal the 200/250/300/350/400 manifest. Do not run the formal matrix before
those gates pass.
