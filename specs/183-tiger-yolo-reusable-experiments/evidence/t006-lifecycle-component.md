# T006 lifecycle component checkpoint

## Runtime plan vs carrier correction

V3 PLAN_SEALED emits the finalized PlanSealerV3 execution digest, also used in
assignments and native observations. SealedCollaborationPlan.plan_digest hashes
the outer carrier including assignment payloads; it is a distinct identity.
YOLO numerical records and terminal marker previously used that carrier hash.
AutomaticInferenceHandle now exposes execution_plan_digest from the existing
coordinator metadata binding (strict canonical SHA-256); legacy handles with
no explicit binding fall back to the carrier, as V2 does. YOLO evidence uses
the execution property. Neither carrier nor runtime hash computation changed.
Four isolated property tests and updated real production-tail numerical tests
pass; full public native handle construction remains T008. Public dependency
projection is still pending and must use this corrected execution binding.

Expanded regression: 591 passed in 46.51s (previous six selectors plus
tests/python/test_spec183_execution_plan_identity.py); JUnit
`Experiments/TigerCluster/results/t006-execution-plan-r1/junit.xml`.

## Request-result join follow-up

collect_request_result now joins strict lifecycle validation with real response
byte reanalysis. It compares GRAPH_READY against caller-authenticated frozen
graph/catalogue digests, then derives request/attempt/plan/result bindings for
numerical reanalysis from the validated lifecycle, not independently supplied
values. Four added tests exercise the real producer numerical function, codec,
NumPy reference and retained bytes, rejecting swapped plan, response or
catalogue identities. The lifecycle in these tests is synthetic; no native
inference is claimed. REQUEST_RESULT_COMPONENT_ONLY still requires native
roles, physical GPU, dependency transfer, model coverage and cleanup checks.

Expanded join regression: 579 passed in 43.59s, same six selectors; JUnit
`Experiments/TigerCluster/results/t006-request-join-r1/junit.xml`.


The validator in `runtime/yolo_result.py` consumes a bounded, non-symlink
`lifecycle.jsonl`. It checks the maintained ten-event schema against externally
provided case/request/attempt/candidate identities. It rejects missing/reordered
events, duplicate JSON keys, nonfinite times, bool-valued counts, malformed
digests, false terminal status and multiple terminal requests. Wall-clock
correction is allowed: sequence establishes ordering.

25 focused tests pass. Test fixtures derive field names and milestone order
from the maintained writer's AST; no native transport or model execution is
simulated as qualification. Return status is `LIFECYCLE_COMPONENT_ONLY`.
The explicit integer timestamp bound also rejects enormous JSON integers
without overflowing float conversion.

Final focused regression: 480 passed in 45.24s on unchanged source after the
timestamp-bound fix; JUnit at
`Experiments/TigerCluster/results/t006-lifecycle-r2/junit.xml` (not committed).
Selectors: `Experiments/TigerCluster/tests`,
`tests/python/test_spec183_v3_backend_selection.py`,
`tests/python/test_spec183_public_recipients.py`,
`tests/python/test_spec180_yolo_numerical.py`, and
`tests/python/test_spec183_numerical_reanalysis.py`.

## Confirmed remaining defect

Update: source fix added. `Yolo26Splitter.describe_candidate_identity` rebuilds
each registered candidate against the same model/graph and requires exactly
one runtime digest match, then returns that verified catalogue entry's ID and
digest. V3 lifecycle emission calls this resolver when an observer is present.
Generic splitters without a resolver retain their existing runtime identity.
No SplitCandidate fields/digest rules or placement/artifact identities changed.
Four AST-isolated production-kernel tests cover mapping, missing/ambiguous
matches and actual V3 emission. Full native planner qualification is pending.
Expanded focused regression: 484 passed in 41.87s; same five selectors above
plus `tests/python/test_spec183_candidate_identity.py`. JUnit:
`Experiments/TigerCluster/results/t006-candidate-identity-r1/junit.xml`.

Additional producer/collector gap confirmed: V3 GRAPH_READY emits graphDigest
only, whereas the maintained lifecycle schema requires catalogueDigest too.
The journal currently allows a subset of allowed fields, so it does not reject
the incomplete event. This must be filled from the verified catalogue owner,
not fabricated by the collector, before T006 closes.

Follow-up source fix: the YOLO builder now retains `catalogue_body_digest` on
its splitter after the existing signature/schema/model/graph/weights checks.
The digest is SHA-256 of the exact canonical signed JSON body (sorted keys,
compact separators, UTF-8, ensure_ascii=False), excluding only the `signature`
envelope. Digest calculation alone is not signature verification. V3 graph
emission propagates this value and rejects malformed nonempty digests. Generic
splitters without catalogues keep their existing optional evidence behavior.
The maintained journal itself is not tightened here: negative/partial traces
can remain partial, but cannot pass the final successful lifecycle validator.

Regression now includes real LifecycleJournal import/write/validate followed
by the new collector (no writer mock), canonical body digest tests including
non-ASCII content and signature-envelope independence, and isolated actual V3
graph emission. Full native adapter construction and model inference remain
unverified. Final collector must compare this digest to the frozen verified
catalogue, not only check its syntax.

Catalogue follow-up regression: 489 passed in 42.77s using the same expanded
six-selector command; JUnit at
`Experiments/TigerCluster/results/t006-catalogue-r1/junit.xml`.

`app_sdk/placement.py` V3 PLACEMENT_DECISION emits
`candidateId=str(selected_candidate.candidate_digest)`. The V3 SplitCandidate
has no catalogue candidate-id field. The maintained User lifecycle observer
forwards this unmodified. In contrast, prepared offer trust and numerical
records bind the catalogue candidate name. The new collector rejects this
mismatch. Do not blindly substitute an environment-supplied label in the
journal: establish a digest-bound catalogue-to-candidate mapping first.

## Still required

- Resolve the candidate identity mismatch without silently changing candidate
  digest semantics or accepting arbitrary candidate labels (source fixed;
  real adapter/planner integration verification remains).
- Verify Selection digest and Provider role/plan/ACK bindings using production
  canonicalization; cross-check numerical reanalysis against this lineage.
- Collect independent native role/node/GPU/edge observations and cleanup/reap
  evidence. User PROVIDER_EXECUTION_STARTED and ARTIFACTS_READY are not that
  evidence.
- Complete T006 orchestration, T007 audit and all real qualification gates.

No SIF build, upload, Slurm submission, or inference run occurred here.
# Retained request join (2026-09-07)

collect_retained_request combines lifecycle/numerical and retained four-role
execution/dependencies. The request ID/index comes from the frozen schedule;
output resolves under node0/user/requests/<index>. Normal cases require
attempt-1, consistent with disabled retry/reselection. User roleDigest must
match the expected Provider map; selected role/provider counts and ACK count
must agree. Selection digest is recomputed exactly as the V3 coordinator:
canonical SHA256 of {"plan": executionPlanDigest, "ack": ackSnapshotDigest}
(app_sdk/placement.py around 4095), not the outer carrier digest.

Runtime candidate digest binds the packaged release/node receipts. Placement
candidate ID/digest binds the chosen catalogue partition/lifecycle/numerical
record. They are distinct parameters, not interchangeable values with the
same label. Native and public dependency evidence use the lifecycle's request,
attempt and execution-plan binding. No component PASS is promoted to an
experiment PASS; GPU/allocation, certified model graph and operator-level
frozen-input coverage still need verification.

Nine tests exercise the actual lifecycle reader; numerical/native boundaries
are explicit doubles. Wrong role/Selection digest, ACK/provider/role count,
request index, attempt and numerical rejection prevent the native join.
They are composition tests, not a real model or transport run.

Expanded focused regression: 680 passed in 44.28s. JUnit:
`Experiments/TigerCluster/results/t006-retained-request-r1/junit.xml`.
