# Provider boundary repair and scoped convergence review — 2026-09-04

## Verdict and scope

**PASS for the scoped Provider repair; NOT a full T014 PASS.**
Current Spec180 still requires a fresh complete candidate source binding,
full design/code convergence, and T015 before SIF/Tiger. This review continues
the native/Python Provider comparison and implements verified discrepancies;
it does not change placement, add a runtime mode, or rerun network experiments.

## Findings and dispositions

| ID | Severity | Source evidence | Disposition / acceptance owner |
|---|---|---|---|
| PB-01 | HIGH | `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py:2172`: failed V3 decoding previously set the projection to None; local preparation and handler execution could continue. | FIXED, T002/T006/T007. A configured V3 issuer or declared V3 envelope fails closed before preparation/status/handler work. Valid legacy envelopes retain their path. Tests mutate missing/malformed/schema/attempt/role/canonical-encoding/size inputs. This proves the callback boundary, not a demonstrated bypass of Core authentication on the live network. |
| PB-02 | MEDIUM | `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py:1402`, `:2289`: every preparation snapshot previously reported attempt 1. | FIXED, T002/T006. The validated V3 attempt reaches accepted, intermediate, failed and ready snapshots. Attempt-2 success and preparation-failure cases pass; legacy remains attempt 1. |
| PB-03 | LOW | `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/provider.py:415` and `app_sdk/facades.py:1032` share a class name but have separate public/admin and network-serving roles. | CLARIFIED. Docstrings explain that from_config constructs one network facade in the same Python process. Public exports/imports are preserved; existing compatibility tests pass. |
| PB-04 | Rejected current-path defect | `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py:695`, `:805`, `:1012`, `:1564`. | The current process vector starts native Providers only; duplicate phases and configured identities already fail. Added negative regressions exercise those guards. No extra process-lock or distributed identity registry is justified by this audit. Manually launched external processes were not tested. |
| PB-05 | Corrected interpretation | `NDNSF-DistributedInference/cpp/ndnsf-di/NativeCanonicalOnnxAssembler.hpp:15`, `.cpp:572`; `native_assembly_helper.py`. | Native owns assignment/cache and invokes the shared Python ONNX assembly implementation. This is an intentional Python preparation dependency, not a second network Provider or independently implemented assembler. |
| PB-06 | Corrected interpretation | `ndn-service-framework/ServiceProvider.cpp:3102`, `:3505`; `specs/116-ndnsf-di-user-api-coherence/contracts/collaboration-operation-status.md`. | Operation status is stored under the Selection digest. Its initial epoch 1 is local to that operation; it must not be replaced with an unrelated ACK attempt or string Provider boot ID. |
| H1 follow-up | Source-binding gap, not a commit-only gate | `contracts/immutable-candidate-v1.md:9`, `:23`; `scripts/spec180_candidate.py:94`; `tests/python/test_spec180_candidate.py:39`. | Corrected the earlier claim that a commit is mandatory. The contract explicitly permits exact modified/untracked bytes. Full current-candidate byte binding is still outstanding; this report's limited hashes do not close it. |

## Traceability and ownership

| Requirement / task | Owner and production path | Focused evidence | Remaining gate |
|---|---|---|---|
| T002/T006/T007, existing V3 Selection boundary | APPProvider.from_config -> network facade -> DistributedInferenceProvider.add_capability_handler -> registered wrapped callback -> centralized ProviderSelectionProjectionV3 validator | `test_ndnsf_di_provider_v3_boundary.py`: malformed input cannot prepare or execute; legacy and valid V3 retain their behavior | Full T014; current Spec180 campaign still uses native Providers |
| Selection attempt/status identity | Validated projection -> preparation reporter -> CollaborationContext.report_operation_status -> Core Selection-scoped status store | Success/failure attempt 2, ordered sequences, epoch 1, legacy attempt 1 | No live Python Provider retry qualification claimed |
| FR-025/T011, one process per configured Provider | Case policy -> process_specs -> native command; duplicate-identity and phase checks before launch | Existing native-only vector test plus two new duplicate rejection tests | Current-source T014/T015 |
| FR-015/T003, source identity | immutable candidate contract -> CandidateRecord dirty-source validation | Existing explicit-dirty and canonical identity tests pass | Refresh the complete candidate and transitive closure |

No new task group, protocol, authority, or assembly algorithm was introduced.
Test/implementation/documentation changes remain in their existing task owners.

## Tests executed

Initial fixture construction needed a positive resource sequence and matching
execution/assembly adapter identities. After correcting the fixture, the
pre-fix run demonstrated **10 failures, 1 pass**: wrong attempt and invalid
Selection reaching execution. Fixture failures are not counted as bug evidence.

After the implementation and added mutation cases:

```text
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper pytest -q \
  tests/python/test_ndnsf_di_provider_v3_boundary.py \
  tests/python/test_ndnsf_di_provider_surface.py \
  tests/python/test_ndnsf_di_app_sdk_compatibility.py \
  tests/python/test_spec180_yolo_minindn.py --tb=short --disable-warnings
106 passed in 1.80s

PYTHONPATH=NDNSF-DistributedInference:pythonWrapper pytest -q \
  tests/python/test_spec180_candidate.py \
  -k 'dirty_source or canonical_identity' --tb=short --disable-warnings
2 passed, 4 deselected in 0.07s

PYTHONPATH=NDNSF-DistributedInference:pythonWrapper pytest -q \
  tests/python/test_spec180_generic_request_api.py \
  -k 'provider_context or legacy_add_role' --tb=short --disable-warnings
7 passed, 11 deselected in 0.43s
```

Total: **115 relevant checks passed**. These are focused repair/compatibility
checks, not the complete local inventory or measured qualification. No native
rebuild was needed for these Python-only behavior changes. No NFD/MiniNDN,
SIF, remote staging, Slurm submission or Tiger run was performed.

## Exact reviewed source checkpoint

HEAD: `286a0098b9bf0dfc2e0b77a320e9e75c38851dc7` plus the existing dirty
working tree. The task did not commit or revert surrounding work. These are
SHA-256 hashes of the reviewed changed source/test files, not a complete seal:

| Repository-relative path | SHA-256 |
|---|---|
| `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py` | `6fbfa90e9b939e788f8318355b6657a87b7114563e369932c243fad6f241b7bb` |
| `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/provider.py` | `49c432a74f022ed654fc55e5eb420f95873953e1b0e93656eb352b1879b77775` |
| `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/facades.py` | `9b3f0a6541105c42fe903d9fa0cd2b1614b65dbd339816d4355035e388b59cfb` |
| `tests/python/test_ndnsf_di_provider_v3_boundary.py` | `c6fb81b0285bc65b9127c9df4a5b8158f25c8d28fcadb4092b508d72edaf94fc` |
| `tests/python/test_spec180_yolo_minindn.py` | `5bd8412bd97fccafe748e9bb0bfbf56bca32890254246452c6f389b1deaae7c5` |

## Readiness, tooling and limitations

| Dimension | Result |
|---|---|
| Intent/necessity | PASS for this repair; speculative process-mode infrastructure was rejected after locating existing guards. |
| Architecture/security/code reality | PASS at the reviewed callback/launch boundaries; real Core authentication stays with Core. |
| Task cohesion/migration | Existing owners retained; no public facade rename or V2 removal; compatibility checks pass. |
| Validation/evidence | Scoped tests PASS; full T014/T015 and current candidate identity remain open. |
| Context Mode | Project health PASS; active authority initially failed on stale audit.md. Reindexed the canonical pointer/guide/spec/plan/tasks plus audit and traceability into the Codex store; final project and strict active health PASS. Repository documents remain authority. |
| CodeGraph | Used first; symbol lookups included archived compare trees and an absolute-path read failed. Canonical source was verified directly before edits. |
| Spec Kit | Prerequisite/strict structure checks PASS: 25 FR, 9 SC, 4 stories, 20 tasks, 4 complete; requirements checklist 25/25. No before/after_implement hooks are configured. |
| GSD | Installation/health PASS. Phase 36 state points to an old Spec170 resume; active Spec180 pointer/tasks control this work. This report and tasks.md preserve the current continuation. |
| ARS | Not applicable to this implementation repair; no literature or experimental-design claim. |

Scoped findings: 1 HIGH and 1 MEDIUM fixed; 1 LOW clarified; two proposed
duplications and the epoch interpretation corrected. No task checkbox or
frozen historical result was upgraded to formal completion.

## Next action

Refresh the complete current source/runtime/harness binding, including this
repair's dirty bytes; perform the full T014 audit against that subject and
require PASS before T015. Only then proceed to the existing SIF/Tiger gates.
Do not repeat the Provider design review or add another Provider runtime mode
as a prerequisite.
