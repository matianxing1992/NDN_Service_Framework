# T006 lifecycle component checkpoint

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
