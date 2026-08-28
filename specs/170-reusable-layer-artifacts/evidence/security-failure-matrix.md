# Spec170 security and lifecycle failure matrix (2026-08-19)

This matrix is an evidence index for the current candidate. `PASS` means the
named case was actually exercised and the expected fail-closed outcome was
observed. `PARTIAL` means a related lower-level check exists but does not cover
the complete requirement. `MISSING` means no qualifying evidence is present.
The matrix is intentionally conservative and does not authorize T029.

## Executed evidence

| Fault or invariant | Result | Evidence | Required terminal observation |
|---|---|---|---|
| Wrong collective epoch/capability | PASS | `DistributedInferenceCollectiveRuntime` | rank event rejected; no start |
| Unauthenticated/local-not-ready rank | PASS | same focused group | input readiness rejected |
| Duplicate/non-advancing input/progress | PASS | same focused group; 50-seed corpus | event rejected; no duplicate completion |
| Whole-group rank failure | PASS | Worker integration regression; 50 rank-failure seeds | `FAILED`, no later rank completion |
| Whole-group cancellation | PASS | focused collective runtime case | `CANCELLED`, later progress rejected |
| No-progress timeout | PASS | focused collective runtime and DATA_V1 tests | `STALLED`/`NDNSF_DATA_V1_NO_PROGRESS` |
| Hard deadline | PASS | focused collective runtime and DATA_V1 tests | `HARD_DEADLINE`/`NDNSF_DATA_V1_HARD_DEADLINE` |
| Real CPU ONNX two-rank adapter execution | PASS (local) | `onnx-cpu-collective-local-20260819.md` | both ranks produce numerical output and complete one group |
| Current-source production D2b with real CPU ONNX | PASS (local) | `production-d2b-onnx-cpu-local-20260819.md` | two Provider handlers execute three roles and publish exactly one Response |
| Current-build post-Selection assignment/request/response ingress | PASS (local) | `spec170-postselection-integration-rerun-20260819.md` | 4/4 cases: assignment fetch, device-mismatch rejection, four-role response, and missing-backbone cancellation |
| Current-build full native core-flow suite | PASS (local) | `spec170-core-flow-full-rerun-20260819.md` | 26/26 cases covering D2a, D2b SVS faults, D2h mappings, capability tamper, and four-Provider role split |
| Current-source production D2b tampered group capability | PASS (local) | `production-d2b-onnx-cpu-local-20260819.md` | Selection fails before native execution; zero final Responses and no timeout |
| Current-source production D2b tampered DATA_V1 SVS segment | PASS (local) | `production-d2b-onnx-cpu-local-20260819.md` | SVS returns the opaque wire; consumer `decodeSegment`/`openSegment` rejects the mutated inner segment |
| Current-source production D2b DATA_V1 SVS segment drop | PASS (local) | `production-d2b-onnx-cpu-local-20260819.md` | selected segment loss remains bounded; no complete fetch is accepted |
| Current-source production D2b DATA_V1 SVS duplicate | PASS (local) | `production-d2b-onnx-cpu-local-20260819.md` | duplicate outer delivery is idempotent; plaintext is reconstructed exactly once |
| Current-source production D2b DATA_V1 SVS reorder | PASS (local) | `production-d2b-onnx-cpu-local-20260819.md` | reordered outer delivery is accepted only after exact sequence reconstruction |
| Exact r23 SIF D0/D1 dependency lifecycle | PASS (bounded, repeated) | `exact-sif-network-rerun-20260819.md`; `spec170-exact-sif-repeat-20260819.md` | Explicit Apptainer 1.5.3 gate; three earlier plus five post-parser-fix sequential D0/D1 blocks (16/16) complete Request → ACK → Selection → Response with all four dependency edges and zero residual Providers |
| Real MiniNDN 3C positive mappings `[1,2,1]` and `[2,1,2]` | PASS (local) | `real-minindn-hybrid-cpu-20260819.md` | both real post-Selection CPU paths complete the oracle with no fallback and exact dependency closure |
| DATA_V1 ciphertext tamper | PASS | `DistributedInferenceCrossProviderGroup` | open/accept throws; no output |
| DATA_V1 epoch/manifest mutation | PASS | cross-provider group tests | consumer rejects mutated operation |
| DATA_V1 replay/duplicate | PASS | cross-provider group tests | duplicate is idempotent; conflicting replay rejects |
| DATA_V1 segment drop/reorder | PASS | 50 fixed seeds | bounded no-progress failure or complete reorder |
| DATA_V1 plaintext wire exposure | PASS | 50 fixed seeds | plaintext absent from encoded segment |
| Current-build cross-Provider DATA_V1 unit suite | PASS (local) | `spec170-cross-provider-unit-rerun-20260819.md` | 9/9 cases, including RSA key wrapping, manifest/terminal mutation, replay, deadlines, and 50-seed faults |
| Peer mismatch | PASS | Tiger r23 job 201040 | no accepted cross-provider operation |
| Replay negative | PASS | Tiger r23 job 201041 | replay rejected |
| Partial assignment/output | PASS | Tiger r23 job 201042 | partial result rejected |
| Hybrid missing-data negative | PASS | Tiger r23 jobs 201045/201046 | missing dependency fails closed |
| Unauthorized NAC-ABE large-data fetch | PASS | MiniNDN large-data gate | unauthorized Provider produces no plaintext |
| Bounded missing-large-data fetch | PASS | `GenericDynamicApi/PreparedAndMessages` | one shared deadline; no 30 s + 30 s hang |
| Terminal epoch-key access after cancel/fail | PASS (local) | `DistributedInferenceCrossProviderGroup/CoordinatorRejectsManifestMutationAndTerminalReuse` | `epochKeyForProvider()` rejects both terminal states; no re-unwrap after key cleansing |
| Cross-Provider epoch-key access | PASS (local) | `DistributedInferenceCrossProviderGroup/CoordinatorUsesRsaWrappedEpochKeyAndWireBundle` | a runtime bound to P1 rejects access to P0's wrapped-key target |
| V2/V3 separation and explicit V2 path | PASS (contract) | Python Spec170 suite | V3 does not silently fall back to V2 |
| Python/native V3 contract suite | PASS | Host glob 105/10; exact-SIF glob 111/6; `spec170-python-contract-rerun-20260819.md` | no unexpected assertion failures |
| Registered authorization selector registry | PASS (local) | `current-python-negative-regression-20260819.md` | 23/23 selector, boundary, freeze/mutation, assignment, admission, lease, and cancellation checks execute successfully |
| Current-source frozen baseline inventory | BLOCK | `current-python-negative-regression-20260819.md` | 77/78 auxiliary negative tests pass, but eleven registered source hashes drift from the frozen subject |

The latest local native regression totals for this matrix are 496 unit cases /
60,009 assertions and 34 integration cases / 480 assertions. The integration target
must be run serially because the legacy fixture uses a shared default PIB; a
parallel unit+integration invocation aborted with `database is locked` and is
not counted as evidence.

The exact-SIF D0/D1 repeat record preserves earlier combined-run D1
`dependency status=incomplete` observations. The consumer fetched the expected
bytes and the response succeeded; one producer timing line was malformed or
interleaved. The parser now handles the Python actual-name format, and the
source logger now serializes timing output, but r23 predates that C++ change.
Until a new source-bound SIF verifies the logger repair, exact-SIF
repeatability remains a bounded PASS with an evidence-integrity warning rather
than a deterministic release claim.

## Remaining gaps

| Requirement | Result | Why it remains open |
|---|---|---|
| Complete T018 signed operation-manifest/key-wrap/zeroization corpus | PARTIAL | terminal epoch-key re-unwrap is now fail-closed locally; production D2b rejects a tampered capability and mutated SVS DATA_V1 segment, while the complete cross-Provider segment fault/key-wrap/zeroization lifecycle remains unwired |
| Complete 3A numerical oracle | PARTIAL | real CPU ONNX two-rank adapter oracle and one current-source production D2b lifecycle pass locally; capability, tamper, drop, duplicate, and reorder bridge cases are covered, but CUDA and full oracle coverage remain absent |
| Complete 3C mutation corpus | MISSING | real positive `[1,2,1]`/`[2,1,2]` paths now pass, but no qualifying end-to-end mutation run covers every omitted/duplicate/wrong redistribution and cancellation case |
| H1-H10 and SC-001..SC-035 traceability | PARTIAL | named rows are indexed, but unchecked task rows and missing artifact hashes remain |
| T029 frozen candidate | MISSING | `frozen-candidate.json` and `freeze-report.md` do not exist; current r23 is bounded verification only |
| T036 performance optimality | MISSING | no three clean-start P01-P05 cold/warm blocks, hierarchical bootstrap, TOST, or Holm analysis |

## Verdict

The current evidence proves meaningful protocol and negative qualification, but
not final protocol completeness, full failure coverage, or performance
optimality. Any claim stronger than that is unsupported by the retained
evidence.
