# Spec 170 protocol-closure audit

The bounded local collective-runtime regression is recorded in
[`collective-runtime-local-20260819.md`](collective-runtime-local-20260819.md).
The latest current-source full local rerun is recorded in
[`current-local-regression-20260819.md`](current-local-regression-20260819.md).
It upgrades the local state-machine qualification evidence but does not replace
the production integration, full negative lifecycle corpus, or T029 freeze
requirements below.
The detailed negative index is in
[`security-failure-matrix.md`](security-failure-matrix.md), with the explicit
pre-freeze decision in [`pre-freeze-closure.md`](pre-freeze-closure.md).

## Current qualification evidence

The Tiger rows below are historical evidence from the sealed r23 source. They
remain useful for qualification, but the uncommitted working tree is not
sealed by r23; no SIF for those changed bytes has been promoted.

| Layer | Result | Evidence |
|---|---|---|
| Cross-Provider positive path | PASS (historical r23) | r23 Tiger job 201039; two-provider CUDA runtime, `NDNSF_DATA_V1` |
| Cross-Provider negatives | PASS (historical r23) | r23 jobs 201040 peer-mismatch, 201041 replay, 201042 partial |
| Hybrid `[1,2,1]` | PASS (qualification) | r23 job 201045; response and numeric oracle error 0 |
| Hybrid `[2,1,2]` | PASS (qualification) | r23 job 201046; response and numeric oracle error 0 |
| Hybrid missing-data negative | PASS | jobs 201045 and 201046 |
| Local C++ integration gate | PASS | 34 cases, 480 assertions (latest current-source rerun) |
| ProviderRoleWorker collective release/failure path | PASS (local) | 2-rank worker integration regressions |
| Real CPU ONNX two-rank collective adapter path | PASS (local) | `onnx-cpu-collective-local-20260819.md`; numerical output and CPU evidence |
| Current-source production D2b with real CPU ONNX | PASS (local) | `production-d2b-onnx-cpu-local-20260819.md`; ACK/Selection, two Provider handlers, three evidence records, one Response |
| Current-source production D2b capability tamper | PASS (local) | `production-d2b-onnx-cpu-local-20260819.md`; Selection rejected before native execution, zero Response |
| Current-source production D2b DATA_V1 SVS tamper | PASS (local) | `production-d2b-onnx-cpu-local-20260819.md`; opaque wire is fetched, consumer `decodeSegment`/`openSegment` rejects the mutated inner segment |
| Current-source production D2b DATA_V1 SVS drop | PASS (local) | `production-d2b-onnx-cpu-local-20260819.md`; selected segment loss remains bounded and does not produce a complete fetch |
| Current-source production D2b DATA_V1 SVS duplicate | PASS (local) | `production-d2b-onnx-cpu-local-20260819.md`; duplicate outer delivery reconstructs the plaintext exactly once |
| Current-source production D2b DATA_V1 SVS reorder | PASS (local) | `production-d2b-onnx-cpu-local-20260819.md`; reordered outer delivery reconstructs the exact plaintext |
| Delayed post-certificate cancellation | PASS (host MiniNDN diagnostic) | explicit 12 s no-progress bound preserves the accepted terminal result and rejects stale/late cancellation |
| NAC-ABE large-data MiniNDN gate | PASS | `results/spec170-large-data-minindn-20260819/summary.json` |

Current r23 hybrid jobs report `onnxruntime-cuda`, `realCompute=true`, and
`cpuFallbackUsed=false` for both Providers. The two mappings therefore have
current-r23 execution evidence rather than relying on the older r22 runs.

## Focused unit coverage

The following targeted groups passed in the current build:

| Group | Cases | Assertions |
|---|---:|---:|
| `DistributedInferenceCrossProviderGroup` | 9 | 722 |
| `DistributedExecutionConsistency` | 3 | 13 |
| Native assignment/selection projection subset | 4 | 10,727 |
| `GenericDynamicApi/CryptoAndAuthorization` | 15 | 10,114 |
| `GenericDynamicApi/TokensAndReplay` | 10 | 243 |
| `GenericDynamicApi/SelectionStrategies` | 22 | 324 |
| `GenericDynamicApi/TargetedInvocation` | 23 | 192 |
| `GenericDynamicApi/AllSelectedAndWorkers` | 12 | 87 |
| `GenericDynamicApi/DeploymentControl` | 8 | 58 |
| `GenericDynamicApi/CollaborationStatus` | 7 | 86 |
| `GenericOpaqueSelection` | 11 | 71 |

The real NAC-ABE large-reference helper validation also passes as a bounded
host-NFD diagnostic (`examples/run_large_data_helper_validation.sh`): an
8,204-byte plaintext is published as two encrypted segments, an authorized
Provider reconstructs the exact plaintext, and an unauthorized Provider
fails closed. The helper now starts a bootstrap Provider before the User (the
User needs the DKEY during construction), and its fetch probe keeps the Face
event loop alive while the synchronous fetch runs. This is real NAC-ABE
coverage, but it remains host-NFD evidence rather than the MiniNDN formal gate.

The corresponding MiniNDN gate now passes with the current build:
`Experiments/NDNSF_LargeData_NacAbe_Minindn.py` publishes an 8,217-byte
plaintext as two encrypted segments, reconstructs the exact plaintext through
the authorized Provider, and obtains a bounded clean failure from the
controller-authorized-but-`/HELLO`-unauthorized Provider identity. The run uses
the wired MiniNDN NFDs and disables adaptive admission; its compact summary
and logs are in `results/spec170-large-data-minindn-20260819/`. This closes
the formal gate for this large-data path, but it does not imply that every
Spec 170 task or the T029 freeze is complete.

`GenericDynamicApi/PreparedAndMessages` initially exposed a real boundedness
defect in the missing-large-data case: SegmentFetcher and the legacy NAC-ABE
fallback each waited on an independent 30-second budget. The implementation
now uses one shared deadline, configurable through
`NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS` (default 30 s). The focused regression
with a 100 ms budget passes, and the complete group now passes **14/14 cases,
135 assertions**. The previous 25-second hang was therefore a reproduced and
fixed protocol failure, not an unverified test gap.

## Full local regression rerun (2026-08-19)

With the current build, the complete local C++ suites were rerun without test
selection:

| Command | Result | Binary SHA-256 |
|---|---|---|
| `taskset -c 3 ./build/unit-tests --report_level=short --log_level=error` | 496/496 cases, 60,009/60,009 assertions | current build; controlled CPU affinity removes unrelated UAV scheduler jitter |
| `./build/integration-tests --report_level=short --log_level=error` | 34/34 cases, 480/480 assertions | current build, serial run |

The current-source production D2b CPU-ONNX lifecycle also passes `1/1` case and
`36/36` assertions; the tampered-capability production negative passes `1/1`
case and `8/8` assertions. The production DATA_V1 SVS positive and tamper
negative pass `1/1` cases with `12/12` and `9/9` assertions respectively. The
production SVS drop, duplicate, and reorder cases pass `7/7`, `13/13`, and
`13/13` assertions. The complete focused integration suite passes `26/26`
cases and `387/387` assertions when the fixture is enabled. This
strengthens the local qualification result, but it does not expand the scope
of the remote Tiger gates, the T029 freeze, the T016/T028 distributed fault
corpus, or the missing formal performance analysis.

The delayed post-certificate cancellation gate also passes on the current host
MiniNDN diagnostic path. With a 5 s injected role delay and an explicit 12 s
DATA_V1 no-progress bound, the current request publishes all dependency
objects, stale and late cancellation are rejected, and the accepted terminal
result is preserved. The exact command and logs are in
[`real-minindn-cancellation-filterfix-20260819.md`](real-minindn-cancellation-filterfix-20260819.md).

The `DistributedInferenceCrossProviderGroup/FixedFiftySeedDataV1FaultMatrix`
case is the new deterministic 50-seed local matrix (657 assertions). It
exercises segment reordering, one dropped segment with bounded no-progress
termination, duplicate suppression, ciphertext tampering rejection, nonce
uniqueness, and a wire-level plaintext search. It is stronger local evidence
for the DATA_V1 failure boundary, but it is not a substitute for the T016/T018
real 3A oracle or the full T028 lifecycle corpus.

The complete Spec170 Python contract suite also passes when the repository
wrappers are placed on `PYTHONPATH` explicitly:

```text
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:Experiments:pythonWrapper \
python3 -m pytest -q -rs tests/python/test_spec170_*.py
103 passed, 9 skipped in 5.30 s
```

The explicit path is part of the reproducible command because the integrated
flow imports `py_repoclient`; an unconfigured host Python path is not evidence
of a protocol failure.

The latest exact-SIF contract rerun, after adding the Provider process-census
and dependency-trace parser guards, reported **111 passed, 6 skipped, 1 warning
in 29.47 s**. The remaining
skips are opt-in NativeTracer/Qwen environments; this count does not close the
remote lifecycle, T028, T029, or T036 requirements.

The exact r23 D0/D1 dependency gate was then repeated in three sequential
blocks and five further sequential blocks: **16/16 workload cases passed**,
with all dependency edges complete and zero remaining Provider processes after
every block. The repeat record is
[`spec170-exact-sif-repeat-20260819.md`](spec170-exact-sif-repeat-20260819.md).

## Remaining protocol gaps

1. The current Tiger runs qualify the exercised positive/negative paths but
   were not performed after a formal T029 freeze; they do not close every
   unchecked task in `tasks.md`.
   The task ledger still marks the pre-freeze T002--T031 work and T033--T039
   closure work unchecked (T032 is the only checked remote task), so this
   audit must not be reported as Spec 170 completion.
2. The D2h workload evidence itself says it does not independently establish
   the complete signed operation-manifest/segmented-Data T018 contract.
3. Negative coverage is strong for peer mismatch, replay, partial assignment,
   and missing data, but it is not an exhaustive lifecycle-fault matrix.
4. The real NAC-ABE large-reference path now has both a successful host-NFD
   diagnostic and a MiniNDN formal gate. The local mock remains intentionally
   unable to produce wrapped NAC-ABE MessageKeys, so it is not used as a
   substitute for this result.
5. T016 local state-machine, real CPU ONNX adapter, Worker integration, one
   current-source production D2b lifecycle, and terminal epoch-key access
   denial now exist, and T028's
   `security-failure-matrix.md` records the named negatives. The matrix is
   still partial: the wider T018 key-wrap/zeroization lifecycle, full 3A
   numerical oracle, full 3B/3C lifecycle
   mutations, T029's `frozen-candidate.json`/`freeze-report.md`, and the formal
   T036 performance analysis remain absent.
6. The auxiliary authorization/security regression is not source-clean: 77/78
   tests pass, while eleven entries in the registered baseline inventory differ
   from the current tree. This is an additional exact-source freeze blocker,
   recorded in `current-python-negative-regression-20260819.md`.

**Verdict:** the current local source has positive qualification evidence
and meaningful negative coverage, but “final protocol completeness” is not yet
verified. The missing groups above must be closed or explicitly scoped out
before marking the objective complete.
