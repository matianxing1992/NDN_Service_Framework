# Spec180 Traceability Matrix (revision 124)

## Revision 123 runtime-readiness correction

The Controller PUBPARAMS startup race is owned by the reusable Core/Python
startup boundary, not by ACK disposition or the application planner:

| Mechanism | Requirement/criterion | Owner | Closing evidence |
|---|---|---|---|
| Core `ServiceController::start()` waits for real `PUBPARAMS` Data; Native/Python Controller propagates readiness failure | FR-013, FR-015, SC-004, SC-005 | T013 implementation repair; T014 convergence; T015/T016 execution consumers | `evidence/t013-controller-pubparams-readiness-current-20260904.md`, focused 22-test pass, current `-j2` build and extension import |

This repair is implemented and wired, but it is not a local/SIF/Tiger result.
It invalidates the revision-121 exact-SIF candidate under the change-plane
rules; T014 and T015 must be rerun from the repaired source before a new SIF.

## Active Tiger-MVP matrix

| Gate | Controlling requirements | Owner tasks | Required closing evidence |
|---|---|---|---|
| S0 native closure | FR-013, FR-015 | T011-NATIVE | one `-j2` build identity; matching `libndn-cxx` path/hash/SONAME/Build ID for NFD, NDN-SVS, NDNSF, NAC-ABE, Python; import/startup PASS |
| S1 candidate | FR-001, FR-002, FR-008, FR-009, FR-015, FR-022, FR-023 | T004--T010 | signed current-role package, experiment trust set, Provider offer keys, encrypted input reference, object/policy/key digests, full-model oracle |
| S2 local execution | FR-003--FR-011, FR-014, FR-022--FR-025 | T002, T005--T011 | live MiniNDN Y-A and Y-B terminal Responses plus critical Y-N controls and cleanup |
| S3 convergence/local freeze | FR-013--FR-015, FR-019 | T014, T015 | code-aware audit PASS and one YOLO-relevant local qualification manifest |
| S4 exact SIF | FR-015, FR-016, FR-018, FR-019 | T013, T016 | one SIF hash, in-image ABI/import/runtime closure, exact-SIF Y-B Response |
| S5 Tiger | FR-016--FR-021 | T017, T018, T020 | one-node/one-GPU/four-Provider 1/1 YOLO result and one structured final verdict |
| Deferred boundary | FR-012, SC-007 | T012, T019 | Qwen handoff preserved; no Qwen, multi-GPU, warm-cache, or performance claim |

Only this table is the active completion map. The historical cross-model table
below explains earlier design work but does not add Spec180 acceptance gates.

**Current audit pointer (documentation revision 124, 2026-09-05)**:
the `Controlling 2026-09-05 correction` section at the top of `audit.md`,
the revision-124 queue in `tasks.md`, and the companion report-only review
in `evidence/audit-revision123-design-code-conformance-20260904.md`. The
current source has focused real-NFD Y-A and Y-N-I live records
(`evidence/t011-y-a-live-current-20260904.md`,
`evidence/t011-y-n-live-current-20260905-r42-i-only.md`); these are not
T014/T015 or qualification evidence, and `qualificationReady` remains false
until S0--S5 pass. The controlling completion item is the FR-008
protected-grant subsystem (T014 BLOCK, owned by T007/T010); the
revision-121 S4 exact-SIF result is invalidated by revision 123.

Current Provider-boundary follow-up:
`evidence/provider-boundary-repair-20260904.md` maps T002/T006/T007 to the
Python V3 rejection/progress repairs and T011/FR-025 to the existing
native-only, duplicate-identity/phase guards. Its 115 focused checks do not
close T014. H1 is current source binding, not a commit-only prerequisite; use
the explicit dirty-source path in `contracts/immutable-candidate-v1.md` when
the qualification subject includes modified or untracked bytes.

Latest Tiger-path follow-up: `evidence/t014-tiger-path-audit-20260904.md`
records T014 **BLOCK**, 118 focused checks, and a 406-file source-only checkpoint.
FR-010/017/018 now map to native Tiger argument rendering and per-role GPU
validation; FR-019/024 map to full inventory and oracle-content validation.
These helpers do not prove the launcher's production result/cleanup chain.
T011 live Y-N, T013 supervision/result wiring, sealed launcher identity, and
complete candidate-plane binding remain controlling gaps.

Latest supervision continuation: `evidence/t013-supervision-repair-20260904.md`
maps T013/FR-019/024 to runtime group supervision and observed terminal
validation, and FR-015/016 to the same probed/executed in-image scripts.
44 focused tests pass; these do not prove live numerical/CUDA oracles,
transient-helper/secret cleanup, Y-N or full candidate closure. T014 remains
BLOCK. The latest source-only checkpoint contains 408 files, not 406.

Latest numerical continuation: `evidence/t013-numerical-repair-20260904.md`
maps FR-011/T010 to `adapters/yolo/reference.py` (registered preprocessing,
reference validation, canonical comparison), FR-019/T013 to User's
`_record_yolo_numerical_result()` before success, and FR-015/016 to fixed-fixture
source inclusion and the native Tiger input flag. The component schema is
`contracts/yolo-numerical-evidence-v1.md`; 83 focused checks pass. Actual model
equivalence, CUDA execution, terminal collection, full cleanup, Y-N and complete
candidate binding remain unverified or unimplemented. No new seal was made;
the prior 408-file snapshot does not bind this changed runtime. T014 is BLOCK.

Latest native-evidence continuation: `evidence/t013-native-evidence-repair-20260904.md`
maps FR-018/T007 to CUDA-selected-device PCI/driver UUID queries and ONNX
first-request profiling; FR-019/T013 maps to Worker-bound process/request/cache
observations and the original per-role executable observer record. Contract:
`contracts/native-execution-evidence-v1.md`. 99 focused checks pass, including
compiled C++ helpers with synthetic CUDA libraries; no real CUDA/network
execution is proved. The terminal collector, full cleanup, Y-N and complete
candidate binding remain controlling gaps. T014 remains BLOCK.

Revision 108 identifies the historical blocking path: `run_minindn_case()` now
has a barriered startup/publication path and the maintained User emits the
bound lifecycle journal, but it cannot produce live evidence until a fresh
`DetectShard0/1` package and Provider offer/model inputs are sealed. It also removes
the SIF/QWEN-F dependency cycle by moving all SIF/release implementation and
the production QWEN-F entrypoint into T013 before T014. T015--T019 are now
execution-only gates for one frozen candidate.

Missing candidate, signature, case-policy, Provider-key, or Y-A model inputs
are external-input blocks. They must be supplied and sealed before T011-A;
temporary packages, offline snapshots, and historical Spec175 evidence cannot
advance the matrix.

The only active owner action is `G0-NATIVE`, followed by `G0-CANDIDATE`. Once
both pass, run one atomic Y-A request to a terminal Response, then reuse that driver for Y-B/Y-N;
do not open another audit loop or build SIF/Tiger while G0/G1 is open.

The same recheck closed a Controller command-path defect: Spec180 publication
now selects its controller-owned APP publication mode from the publication
file alone, without the unrelated repository-deployment manifest. This is a
wiring correction, not live-case evidence.

The Controller also validates the publication envelope before signing, including
signer-scoped names, candidate/package/catalogue digests, uniqueness, bounded
metadata, and per-artifact digests. The mutation regression is seam evidence;
exact APP readback and a terminal Response remain required.

Revision 110 adds an explicit pre-network owner for the native closure. The
Python extension, NDN-SVS, NAC-ABE, and NFD must resolve one identical hashed
`libndn-cxx`; a split closure is recorded as `WAITING_EXTERNAL_INPUT` and is
not evidence against the protocol. This gate closes before candidate sealing
and before any MiniNDN process is created.

Revision 105 also narrows the active work to G0/G1/G2. Open task status is not
parallel authorization: the atomic `FullModel` Y-A path must produce one live
terminal Response before shared-role, negative-matrix, SIF, or Tiger work can
advance. This is a sequencing rule, not a new protocol requirement.

Iteration 91 adds fail-closed runtime-catalogue integrity checks: exact
candidate snapshot coverage, exact case role/artifact-name coverage, unique
absolute artifact Data names, and a required controller-rooted signer
certificate on APP readback. These checks are adapter evidence only and do
not count as live publication or protocol qualification.

Iteration 88 adds a runner-owned publish-and-exact-readback seam for the
signed runtime snapshot. The production driver must still invoke it after
Provider readiness and before User startup.

Iteration 89 adds instance-owned, idempotent runtime teardown. The production
driver must invoke `MiniNdnCaseRuntime.stop()` from `finally`; this closes only
the adapter cleanup seam and does not count as live case evidence.

Iteration 90 makes that teardown one-shot and rejects runtime restart after
cleanup, preventing duplicate failure handling from touching another case's
MiniNDN state.

Iteration 87 also closes the parser-boundary gap for non-ACTIVE snapshots:
retired and revoked records cannot be used for new placement.

Iteration 86 also closes the parser-boundary gap for duplicate candidate
digests: the signed runtime snapshot resolver now enforces one record per
candidate before placement.

Iteration 85 adds the V3 artifact-authority regression: an explicit signed
active catalog now goes through `_prepare_artifacts` before Selection. It also
keeps the runtime catalogue publication gap explicit: the package candidate
catalogue and the active snapshot are separate records, and T011 owns
candidate-bound role/rank artifact publication plus the signed active snapshot
receipt before starting User.

| Requirement / criterion | Owner tasks | Formal cases / evidence |
|---|---|---|
| FR-001, FR-002 / SC-003 | T001, T004, T005, T008 | export/adapter/catalogue-trust-root-v1 plus trust-root-registry signature/equivalence focused tests; Y-A/Y-B; YOLO-F |
| FR-003, FR-004, FR-005, FR-006, FR-007 / SC-001, SC-002 | T002, T005, T006, T009, T011 | generic API, network `CollaborationAckClosed` authority, Trust-Schema-validated ACK signer provenance projected through ServiceUser/pybind, candidate-bound Provider-identity offer verification, authenticated artifact-snapshot resolution, migrated application, per-candidate feasibility, signed-priority/digest tie-break, and `Y-N-O` catalogue-order control; Y-A/Y-B/Y-N (with `Y-N-C` making both candidates infeasible) |
| FR-008, FR-009, FR-023 / SC-002, SC-009 | T007, T010, T011 | role-kind parity, native assembly/cache/security tests; Y-N; exact-SIF/Tiger lineage |
| FR-010, FR-011, FR-012 / SC-003, SC-004, SC-006, SC-007 | T008, T009, T010, T011, T012, T016, T018, T019 | model-neutral runtime, YOLO oracle, frozen tiny CPU/MiniNDN Q-C/Q-W reference manifest, and signed external artifact-manifest verification; local CPU backend is recorded, Tiger CUDA backend is mandatory |
| FR-013, FR-014 / SC-004 | T001, T014, T015 | contract gate, audit PASS, and one current local qualification manifest |
| FR-015, FR-016 / SC-005 | T003, T013, T016, T017 | mutation tests, candidate closure, in-image ABI/import/library manifest, one exact-SIF YOLO Y-B replay plus explicitly non-qualifying `Qwen-runtime-smoke` (never Q-C/Q-W/QF1/QF2) |
| FR-017, FR-018, FR-019, FR-020 / SC-006, SC-007, SC-008 | T013, T017, T018, T019, T020 | fixed Tiger profile, one cold Y-B request, at most one byte-identical infrastructure resubmission, terminal closure |
| FR-021 | T020 | final claim/evidence-language audit |
| FR-022 / SC-009 | T002, T009, T010, T011, T013, T017, T018, T020 | inline/reference API tests; encrypted repo input and selected-role fetch; redaction/security negatives; Y-A/Y-B/Y-N/YOLO-F |
| FR-024 / SC-004 | T001, T013, T015 | `contracts/local-suite-inventory-v1.md`, source-bound inventory, one-child-per-item execution, and complete status/cleanup manifest |
| FR-025 / SC-004, SC-009 | T011, T014, T015 | `contracts/yolo-minindn-runner-v1.md`, reuse of the maintained MiniNDN/NFD/SVS startup/routing/keychain/process-supervision helpers, fixed Y-A/Y-B/Y-N Provider cardinality and distinct capability-cover preflight, candidate-bound role policy/trust inputs, fail-before-start checks, coordinator-bound exactly-once request/attempt lifecycle emission (wired, live trace pending), instance-owned idempotent `MiniNdnCaseRuntime.stop()` teardown, fixed `Y-N-O` plus `Y-N-C/P/R/I/E/L` outcomes, ACK-to-Response oracle, and isolated Y-A/Y-B/Y-N evidence |

## Source-owner status

T001 treats this table as the machine-checkable owner map. `existing` means the
path is required now and must resolve before the contract gate can pass;
`planned` means the path is created by its named implementation task and is not
silently substituted by a similarly named file.

| Path or path group | Owner | Status at Spec180 audit | Required evidence |
|---|---|---|---|
| `specs/175-ndnsf-di-streamed-invocation/handoff-to-spec180.md` and `evidence/local-closure-current.md` | Spec175 T023 / T001 | existing | sealed `LOCAL_FUNCTIONAL_PASS` and source revision |
| `specs/180-ack-driven-cross-model-qualification/contracts/*` and `trust-root-registry-v1.json` | T001/T012/T013 | existing (configured) | contract text, frozen Qwen reference JSON, local-suite inventory schema, candidate-bound Provider-offer policy, schema parse, checked-in trust-root entries and public-key digests |
| `core/protected_artifacts.py`, `security/artifact_policy_authority.py`, `app_sdk/placement.py` grant seam, native grant unwrap/key lifecycle | T007/T010 | existing (scaffolding only: HMAC/`repr` digest, `plaintext-v1` defaults) | signed Provider-recipient-encrypted grant Data from the configured policy authority per Spec170 `artifact-assembly-v1`, plan finalization with grant references, Provider verify/unwrap, non-`plaintext-v1` production epoch, and a real grant mutation behind Y-N-E; HMAC scaffolding cannot qualify (revision 124) |
| `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/*`, `core/*`, `repo_reference.py`, `provider.py`, `ndn-service-framework/ServiceUser.cpp`, `pythonWrapper/src/ndnsf/_ndnsf.cpp` | T002/T006/T009 | T002 partial; T006 partial; T009 partial | Generic request/input envelope, candidate-declared `APPLICATION_INPUT` projection, Provider ingress/terminal ownership guards, packet-backed Trust-Schema ACK provenance projection, compatibility tests, and source-bound publication/binding now pass; `NetworkCatalogSnapshotResolver` obtains artifact metadata through exact-name signed APP Data after ACK closure and verifies a canonical snapshot digest. The legacy `add_role()` wrapper is also invoked after V3 wiring. Live encrypted fetch, production ACK event ordering, native enforcement, certificate-chain offer verification, and maintained caller execution remain open |
| `NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/yolo/*`, `tools/ndnsf-di/*` | T004/T005 | T004 partial; T005 partial | exporter/lock, fixed fixture, 640×640 oracle/repeat, canonical NMS policy, ORT equivalence, graph/candidate adapter, signature/digest, and runtime `SplitCandidate` ingress/egress digest propagation exist; iteration 71 replaces percentage/index cuts with signed architecture-scope node sets, lifted-constant ownership, producer/consumer tensor contracts, dependency edges, semantic safe cuts, and graph revalidation, with an altered-partition regression; test-only unsigned construction remains available only for focused fixtures; registered production consumption, native assembly, and live ACK/application wiring remain |
| `scripts/spec180_candidate.py` | T003 | existing | canonical candidate digest, evidence binding, and invalidation tests |
| `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` native-closure preflight | T011/T013 | implemented (pre-start gate) | resolved-path, SHA-256, SONAME/Build ID, import, and startup-smoke evidence proving one identical `libndn-cxx` closure for NFD, NDN-SVS, NDNSF, NAC-ABE, and the Python extension; split closures remain `WAITING_EXTERNAL_INPUT` |
| `contracts/local-suite-inventory-v1.md`, `scripts/spec180_inventory.py`, `scripts/spec180_release.py`, `scripts/validate_spec180_results.py`, `packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/build-local-sif.sh`, `packaging/ndnsf-di-container/bin/ndnsf-di-pre-tiger-checklist`, `packaging/ndnsf-di-container/jobs/spec180/profile.json`, `submit.sh`, and `run-functional.sh` | T013 | existing (partial) | strict inventory generator with native requester config input identity, fixed profile, dispatcher-schema consumption, explicit Slurm export serialization, host-side signed QWEN-F manifest identity/CUDA/no-fallback check, SIF digest/Apptainer entry boundary, pre-output model/workload digest recheck, submission wrapper, and structured terminal-result validator have focused evidence; remaining SIF/preflight implementation and production result-writer integration must close before T014, while candidate-bound execution belongs to T015--T019 |
| `Experiments/NDNSF_DI_QwenAckDriven_Minindn.py` and `tests/python/test_spec180_qwen_entrypoint.py` | T013 | implemented (partial) | separate external-manifest-bound Qwen3.6-27B ONNX entrypoint, incremental object digest checks, tokenizer/prompt binding, fixed CUDA three-stage delegate, and focused mutation tests; a signed production manifest/object set, in-image execution, and result writer remain required before T014/SIF; T019 only executes the sealed entrypoint |
| `scripts/run_spec180_local_gate.py` and `tests/python/test_spec180_local_gate.py` | T011/T015 | existing (partial) | source-digest preflight, one-child-per-entry supervision, inventory snapshot, PID/oracle/exit/timeout/cleanup records, redacted logs, and no-side-effect mutation tests; real candidate execution remains open |
| `scripts/run_spec180_case.py` | T011/T013 | existing (partial) | thin in-image dispatcher for the remote gates; consumes only the candidate-digest-bound `spec180-dispatch-workload-v1` schema, validates exact gate/case, `/bundle`-relative entrypoint, fixed args, environment allowlist including the explicit native requester config, digest, fixed mounts, and fresh evidence root, then `exec`s the maintained entrypoint; QWEN-F is reserved for the separate Qwen3.6-27B ONNX entrypoint and fails closed until its production manifest/object set is available, so the Spec175 M11 fixture cannot be mislabeled; focused mutation tests pass; it never accepts ambient paths or duplicates/replaces protocol semantics |
| `audit.md`, `evidence/audit-revision123-design-code-conformance-20260904.md`, `evidence/t011-y-a-live-current-20260904.md`, `evidence/revision105-scope-order-correction-20260903.md`, `evidence/revision107-input-status-20260903.md`, `evidence/revision108-delay-diagnosis-20260903.md`, `evidence/revision109-native-closure-diagnosis-20260903.md`, and `evidence/t013-qwen-entrypoint-current-20260903.md` | T014 | existing (current checkpoint plus historical context) | revision-123 design/code review and current-source Y-A record close the former NOT_WIRED finding for implementation; T014 must still inspect the complete production path and return a fresh convergence PASS. QWEN remains deferred, and T015--T019 remain execution-only. This is not convergence or qualification evidence |
| `contracts/yolo-minindn-runner-v1.md` and `contracts/provider-offer-trust-v1.md` | T011/T014 | existing (contract plus focused verifier) | candidate-bound policy/trust/input boundary, semantic safe-cut requirement, closed milestone field allowlist with scalar-only enforcement, and `ProviderOfferTrustVerifier` focused evidence; barriered executable runner and controller publication path are wired, while live certificate-chain callback, ACK-to-Response evidence, and candidate-bound Provider key/model inputs remain open |
| `Experiments/NDNSF_DI_StreamedGeneration_Minindn.py` | Spec175 wrapper / T015 consumer | existing (partial) | maintained Q-C/Q-W wrapper with fixed M01/M11, seed, tiny-ONNX, V3, four-stage, and settle delegation; current execution evidence remains open |
| `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` and the remaining `packaging/ndnsf-di-container/jobs/spec180/*` case-runner paths | T011/T015 | partial | source-bound input/package preflight, fixed Y-A/Y-B/Y-N Provider cardinality and distinct capability-cover validation, explicit runtime node/identity binding, topology/policy-digest rechecks, merged shared-node route planning, side-effect-free Controller/Repo/Provider/User command vector, explicit control/providers/user startup phases with readiness and node preflight, coordinator/User lifecycle-journal transition wiring with explicit request/attempt binding, native `APPLICATION_INPUT` ingress binding, real native Y-N-C capability mutation, and case-owned negative cleanup verification; focused evidence is in `evidence/t011-native-boundary-repair-20260904.md`. Missing/invalid pre-network inputs now report `WAITING_EXTERNAL_INPUT` (exit 78), while live trace, real NFD/NDN-SVS Y-A/Y-B/Y-N execution, candidate-bound inventory materialization, and isolated-suite evidence remain open. Overlapping Provider role advertisements remain permitted when a distinct cover exists, while final one-to-one assignment remains ACK/sealed-plan owned; the real driver must wire controller/repository publication, ACK/Selection/Provider/Response execution, lifecycle evidence, and result evidence; Q-C/Q-W already bind the maintained `NDNSF_DI_StreamedGeneration_Minindn.py` wrapper |
| `packaging/ndnsf-di-container/jobs/spec180/*` qualification execution | T016--T020 | planned | execution only: exact-SIF replay, accepted-byte staging, Tiger execution, and final closure occur only after T013 implementation and T014/T015 pass; these tasks may not modify the candidate |
| `NDNSF-DistributedInference/cpp/ndnsf-di/*` and `tests/{unit,integration,python}/*spec180*` | T006/T007/T008/T010/T015 | T006 partial; T007 partial; later extensions planned | ACK candidate-feasibility, catalogue-order, signed-priority, role-kind, and focused input-integrity/terminal-ownership evidence exist; the shared native-assembly regression also covers external-initializer staging and graph-only rejection; signed-root publication of initializer metadata, native parity, production ACK event ordering, complete security matrix, and isolated-suite evidence remain |

The frozen Qwen reference is specifically `contracts/qwen-reference-manifest-v1.json`
plus `evidence/t012-qwen-reference-current-20260902.md`; it registers identity
and drift controls only. Q-C/Q-W execution is deferred by revision 112 and is
not a current T015 requirement; inherited Spec175 results remain historical.

The `planned` rows are not current implementation evidence. They are required
to become `existing` only when their owner task creates and tests them.

## Evidence levels

- `implemented`: source exists.
- `wired`: production caller reaches it.
- `executed`: the real path completed.
- `measured`: a registered metric/artifact exists.
- `qualified`: every case oracle and identity gate agrees.

No requirement advances directly from `implemented` or `wired` to
`qualified`.
