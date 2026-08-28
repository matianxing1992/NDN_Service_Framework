# Spec170 final audit: protocol, negatives, and performance

The latest bounded local collective-runtime evidence is recorded in
[`collective-runtime-local-20260819.md`](collective-runtime-local-20260819.md).
The latest current-source local rerun is recorded in
[`current-local-regression-20260819.md`](current-local-regression-20260819.md).
The post-inventory local rerun is recorded in
[`current-local-regression-rerun-20260819.md`](current-local-regression-rerun-20260819.md).
The negative-case index and pre-freeze decision are recorded in
[`security-failure-matrix.md`](security-failure-matrix.md) and
[`pre-freeze-closure.md`](pre-freeze-closure.md).
The concise durable verdict and interim requirement index are in
[`closure-report.md`](closure-report.md) and [`traceability.md`](traceability.md).
It is additional qualification evidence; the final objective remains blocked
until production integration, the complete failure matrix, T029 freeze, and
performance evidence are closed.

Date: 2026-08-19  
Sealed bounded-verification candidate: local r23 SIF, Apptainer 1.5.3, SHA-256
`5b8bd6baaaf7288b3b593538b7c7bfa086feeca91276bef56d7b6c03e5ae9eeb`.
The image is bound to source revision
`989a9daace669a4f93496dade3176c527edb2469`; uncommitted working-tree changes
are not sealed, and no SIF for those changed bytes has been promoted yet.

This report is an audit of current evidence, not a completion declaration.
The active task ledger and frozen-candidate contract remain authoritative.

## Executive result

| Axis | Current verdict | Evidence |
|---|---|---|
| Exercised protocol paths | PASS (qualification) | Current-source local 34-case/480-assertion integration gate and 496-case/60,009-assertion controlled unit gate; real CPU-ONNX D2b, current host NativeTracer bundle-materialization rerun (1/1 request, 4/4 dependency edges), capability-tamper, DATA_V1 SVS tamper/drop/duplicate/reorder, and delayed cancellation negatives; Tiger r23 D2b/D2h; MiniNDN NAC-ABE large-data gate |
| Final protocol completeness | **BLOCK / not verified** | T029 is not frozen; T018 and T024–T031/T033–T039 remain unchecked; uncommitted source changes still need an exact-source SIF |
| Candidate source/build coverage | **BLOCK / not verified** | A broader 1,102-file pre-freeze inventory now covers runtime/build/harness/test/job roots, but the source-only archive still omits those broader inputs and T029 has not bound model/security/route/schedule/Gate A/B/C hashes; see `source-seal-coverage-audit-20260819.md` and `spec170-candidate-input-inventory-20260819.md` |
| Negative coverage | PASS for named cases; **incomplete lifecycle matrix** | Peer mismatch, replay, partial, missing-data, NAC-ABE unauthorized, DATA_V1 50-seed, and Worker collective cases pass; full T028/T037 lifecycle matrix remains open |
| Performance optimality | **NOT VERIFIED** | One-Provider corpus shows NDNSF slower; Spec171 adds a matched 10-seed range matrix, and the 40-test claim-boundary regression prevents unsupported optimum claims, but the preregistered repeated analysis remains missing |

The ledger currently contains 39 tasks: T001 and T032 are checked, while 37
remain unchecked. A local or Tiger qualification result therefore cannot be
labeled “Spec170 complete.”

## Protocol evidence

Passed evidence currently covers:

- 34/34 current-source local integration cases and 480/480 assertions;
- the focused `Spec170NativePostSelection` rerun also passed all 4/4 cases,
  including encrypted Selection → assignment fetch → native request/response,
  device-mismatch rejection, and missing-backbone cancellation; see
  `spec170-postselection-integration-rerun-20260819.md`;
- the complete current-build `Spec170NdnsfDiCoreFlow` rerun passed 26/26
  cases, covering D2a/D2b/D2h, SVS faults, capability tamper, and the
  four-Provider role split; see `spec170-core-flow-full-rerun-20260819.md`;
- the independent `DistributedInferenceCrossProviderGroup` unit suite passed
  9/9 cases, including the 50-seed DATA_V1 fault matrix; see
  `spec170-cross-provider-unit-rerun-20260819.md`;
- 496/496 complete local unit cases and 60,009/60,009 assertions, including
  explicit no-progress and hard-deadline termination regressions;
- `GenericDynamicApi/PreparedAndMessages`: 14/14 cases, 135/135 assertions,
  including the shared-deadline missing-large-data regression;
- complete current-source local unit regression: 496/496 cases and 60,009/60,009
  assertions, including a deterministic 50-seed DATA_V1 fault matrix (657
  assertions) and a 200-row collective delay/loss/failure matrix for
  reordering, drop/no-progress, duplicate, tamper, nonce, plaintext-wire,
  whole-group failure, and no-global-barrier checks;
- real CPU ONNX two-rank adapter/collective regression: 8/8 focused cases and
  2,336/2,336 assertions, including numerical output and ExecutionEvidence;
- current-source production D2b real CPU-ONNX lifecycle: 1/1 focused case and
  36/36 assertions, with 26/26 focused integration cases and 387/387
  assertions when the fixture is enabled; the complete native core-flow rerun
  is recorded in `spec170-core-flow-full-rerun-20260819.md`;
- real MiniNDN CPU NativeTracer positive 3C mappings: `[1,2,1]` and `[2,1,2]`
  both pass the post-Selection path with exact dependency closure and a
  numerical oracle; mutation coverage remains open;
- current-source production D2b capability-tamper negative: 1/1 case and
  8/8 assertions, with Selection failure before native execution and zero
  final Responses;
- current-source production DATA_V1 SVS positive/tamper boundary: 1/1 case
  and 12/12 assertions for fetch/open, plus 1/1 case and 9/9 assertions for
  consumer rejection of a mutated inner segment;
- exact r23 SIF CLI/D0/D1 gate: 4/4 tests passed, including explicit
  Apptainer 1.5.3 identity/version selection, with the correctly typed
  four-role external bundle; both D0 four-Provider and D1 single-Provider
  paths completed all four dependency edges and the full request lifecycle
  (bounded evidence only; not current dirty-tree or T029 evidence);
- Tiger r23 D2b positive, peer-mismatch, replay, and partial-output cases;
- Tiger r23 D2h mappings `[1,2,1]` and `[2,1,2]`, including numeric oracle
  results and no CPU fallback in the recorded jobs;
- MiniNDN NAC-ABE large-data path: an 8,217-byte plaintext becomes two
  encrypted segments, the authorized Provider reconstructs the exact bytes,
  and `/example/hello/provider/unauthorized` fails closed;
- the registered authorization selector/boundary/freeze-mutation/admission/
  lease/cancellation checks execute successfully (23/23); this proves the
  registry is runnable, but not that the current dirty source matches its old
  frozen inventory;
- the final SIF/build record identity and local-SIF route are documented in
  `README.md`, `implementation-guide.md`, and the iTiger skill reference.

These results establish exercised behavior. They do **not** independently
prove the complete signed `NDNSF_DATA_V1` contract, all T018 key/nonce/replay
requirements, the T029 freeze, or every FR/SC/H traceability row.

The repository now contains the local `CollectiveRuntime` implementation,
Worker integration regression, and `security-failure-matrix.md`. T029's
`frozen-candidate.json` and `freeze-report.md` remain absent, and the matrix
explicitly records the remaining adapter/transport/lifecycle gaps.

## Negative-coverage audit

| Negative class | Result |
|---|---|
| Cross-Provider peer mismatch | PASS (Tiger 201040) |
| Replay | PASS (Tiger 201041) |
| Partial assignment/output | PASS (Tiger 201042) |
| D2h missing-data negative | PASS (Tiger 201045/201046) |
| NAC-ABE unauthorized large-data fetch | PASS (MiniNDN and host-NFD diagnostic) |
| Missing large-data fetch boundedness | PASS (100 ms focused regression) |
| 50-seed rank-delay/loss/security corpus | PASS for local state/DATA_V1 subsets; PARTIAL for the complete T028/T037 corpus |
| Real CPU adapter rank failure/no-progress path | PASS (local Worker); PARTIAL for production transport |
| Current-source production D2b CPU-ONNX positive lifecycle | PASS (local); full production fault/oracle matrix remains partial |
| Current-source production D2b capability tamper | PASS (local); production SVS DATA_V1 tamper, drop, duplicate, and reorder faults are also covered at this lifecycle boundary |
| Current-source production DATA_V1 SVS segment tamper | PASS (local); opaque wire is fetched, then consumer decode/open rejects the mutated inner segment |
| Current-source production DATA_V1 SVS drop/duplicate/reorder | PASS (local); bounded drop and exact duplicate/reordered reconstruction are covered by three production bridge cases |
| Delayed post-certificate cancellation with stale fencing and terminal preservation | PASS (host MiniNDN diagnostic); injected 5 s stage delay succeeds with an explicit 12 s no-progress bound |
| Current-source authorization/security baseline inventory | BLOCK; 77/78 auxiliary negative tests passed, but eleven registered source hashes drift from the frozen subject |
| Full cancellation/no-progress/stale-output lifecycle matrix | MISSING |

The correct claim is “the named negative cases are covered,” not “all failure
modes are covered.”

## Performance audit

The existing confirmatory one-Provider corpus reports mean successful-response
latencies of approximately 82.57 ms for gRPC, 161.45 ms for NSC, and 403.93 ms
for NDNSF at 10 RPS; mean p95 values are approximately 84.14, 163.74, and
409.08 ms respectively. This is evidence against a universal NDNSF latency
advantage in that condition.

The existing Spec173 NDNSF multi-Provider runs are not matched gRPC/NSC
controls and use different policy/workload cells. A separate registered
Spec171 artifact does contain a matched ten-seed multi-Provider matrix at
100 m/150 m and 2 m/s. It reports mean successful-response latency of
72.55/100.28 ms for NDNSF, 80.33/80.25 ms for sequential gRPC, and
1,039.44/465.69 ms for sequential NSC at the two ranges, with equal success
counts within each range. This gives a conditional descriptive advantage for
NDNSF at 100 m, but the ordering reverses against gRPC at 150 m. An exploratory
ten-seed bootstrap of per-seed means has intervals including zero for NDNSF
minus gRPC at both ranges (−6.52 ms [−44.60, 29.86] at 100 m and +20.03 ms
[−14.89, 52.95] at 150 m). The artifact is descriptive and does not establish
a global optimum; its registered claim gate is
`DESCRIPTIVE_RANGE_SPEED_MATRIX_ONLY` because the lower-bound-positive
advantage criterion was not met.

The separate Spec171 opportunity holdout (seeds 72--81, 100 m, 2 m/s) was
also re-analyzed. Among 1,312 predeclared switch-required requests, the
paired p95 difference was −596.55 ms [−992.18, −200.11] for NDNSF minus gRPC
and −2,618.32 ms [−2,916.27, −2,315.09] for NDNSF minus NSC. Across all
requests, successful-latency means were 106.34 ms (NDNSF), 98.77 ms (gRPC),
and 1,008.30 ms (NSC), so this result supports only a switching-window p95
advantage, not an overall or global optimum. The retained analysis is
[`performance-opportunity-holdout-20260819.md`](performance-opportunity-holdout-20260819.md).

The required three clean-start blocks, P01–P05 cold/warm sequence,
predeclared paired estimand, 10,000-bootstrap interval, TOST, and Holm
correction are still absent.

The claim-boundary and analyzer regression was rerun on the current tree:
40/40 tests passed in 2.30 s (log SHA-256
`a9d1bef43ba9966c410e31a7bde92e3f1c1161386776057f9d73c109fc0c28ba`). This
protects against over-claiming from descriptive artifacts but is not a
performance measurement and does not close T036.

Therefore the defensible performance conclusion is conditional: NDNSF's
multi-Provider mechanisms are qualified for the exercised protocol paths, and
the matched matrix/holdout show range- and switching-window-dependent patterns,
but global performance optimality is not demonstrated.

## Required closure work

1. Finish or explicitly scope T018 and the pre-freeze T002–T028 contract,
   including the signed operation-manifest/`NDNSF_DATA_V1` evidence.
2. Create a T029 freeze record binding source, SIF, locks, bundle, workload,
   routes, and evidence hashes.
3. Close the applicable T030–T039 evidence rows without changing the frozen
   candidate; any source or executable fix requires a new candidate identity.
4. If an optimality claim is required, run only the preregistered paired
   multi-Provider matrix only after a current-source SIF is rebuilt, with a
   disk-retention plan.
   Otherwise retain the negative/conditional performance wording.

Until these items are closed, the final audit status is **BLOCK** for the full
objective, while the named protocol qualification gates remain **PASS**.
