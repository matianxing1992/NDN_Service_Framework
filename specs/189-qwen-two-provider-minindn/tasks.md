# Tasks: Qwen 0.6B Two-Provider MiniNDN Full-Path Validation

**Input**: Design documents from `/specs/189-qwen-two-provider-minindn/`

**Status**: IN_PROGRESS

**Rule**: Production C++ → C++ selector/oracle → Python MiniNDN orchestration. Every task is statically reviewed before dependent work. A task remains `PARTIAL` until its focused C++ behavior and evidence pass; `STATIC_PASS` is not runtime completion.

## Logical Batches

| Batch ID | Members | Stable exit | Shared selector / owner | Dynamic profile | Status | Evidence |
| --- | --- | --- | --- | --- | --- | --- |
| B189-0 | T001 | immutable contract, preflight and symbol/closure map | CodeGraph, `rg`, `nm -C`, `readelf` | none | PARTIAL | `evidence/b189-convergence.md` |
| B189-1 | T002,T003 | `prepare` commits/reuses two layer references | `DI_NativeArtifactAuthority` + Repo C++ selector | asan-ubsan where feasible | PARTIAL | `evidence/b189-prepare.md` |
| B189-2 | T004,T005 | reference-only request and two-placement Selection | `DI_NativeRequester` + Core controller | none | PARTIAL | `evidence/b189-placement.md` |
| B189-3 | T006,T007 | `GRANT_VERIFIED → EXECUTION_ENTERED → FETCH → ASSEMBLY → RUNNER_READY → EXECUTE → TERMINAL` | `di-native-provider`, `DI_NativeOnnxAssemblyWorker`, C++ E2E oracle | asan-ubsan where feasible | BLOCKED | `evidence/b189-execution.md` |
| B189-4 | T008 | measured resource guard and complete drain | C++ counters + Python sampler | none or asan-ubsan | NOT_STARTED | `evidence/b189-resource.md` |
| B189-5 | T009,T010 | repeat verdict and candidate evidence are consistent | Spec189 runner/evidence checker | none | NOT_STARTED | `evidence/b189-convergence.md` |

## Execution Progress

| Unit / Details | Status | Depends | Evidence / Remaining | Updated |
| --- | --- | --- | --- | --- |
| [T001 Freeze candidate and production path](#t001) | PARTIAL | — | candidate/artifact identity frozen; preflight, caller map, build/source closure and handoff identity still pending | 2026-09-18 02:22 -05:00 |
| [T002 Prepare Qwen graph and split packages](#t002) | PARTIAL | T001 | real Qwen graph, initializer and two stage artifacts checked; native prepare oracle pending | 2026-09-18 02:22 -05:00 |
| [T003 Publish/reuse layer references in Repo](#t003) | NOT_STARTED | T002 | one committed manifest, duplicate prepare and source-release C++ evidence pending | 2026-09-18 00:00 -05:00 |
| [T004 Project payload-free request](#t004) | NOT_STARTED | T003 | request wire parser/oracle and oversized-model negative pending | 2026-09-18 00:00 -05:00 |
| [T005 Produce real two-provider ACK/Selection](#t005) | PARTIAL | T004 | real signed offers/Selection observed; C++ placement oracle and no-fetch proof pending | 2026-09-18 02:22 -05:00 |
| [T006 Fetch and assemble selected layers](#t006) | PARTIAL | T005 | grant verification and endpoint fix observed; fetch/assembly/runner counters and regression pending | 2026-09-18 02:22 -05:00 |
| [T007 Execute hidden-state handoff and terminal oracle](#t007) | PARTIAL | T006 | Spec189 C++ oracle is registered, globally built and has positive/negative selector checks; real hidden-state handoff and terminal run pending | 2026-09-18 04:05 -05:00 |
| [T008 Measure resource and drain ownership](#t008) | NOT_STARTED | T007 | RSS/swap/disk guard, cancellation and zero-residue evidence pending | 2026-09-18 00:00 -05:00 |
| [T009 Run the real two-provider MiniNDN candidate](#t009) | PARTIAL | T008 | r01-r21 real runs retained; post-grant first boundary and complete event sequence pending | 2026-09-18 02:22 -05:00 |
| [T010 Repeat, converge and classify verdict](#t010) | NOT_STARTED | T009 | second run, immutable tuple check and final verdict pending | 2026-09-18 00:00 -05:00 |

## Current Checkpoint

**Updated**: 2026-09-18 06:58 -0500

Spec189 has real candidate artifacts, a globally closed and rebuilt affected DI
target set, a registered C++ provider-stage oracle, and real MiniNDN runs
through signed ACK/Selection and protected-grant verification. T001, T002,
T005, T006, T007 and T009 are `PARTIAL`; T003, T004, T008 and T010 remain
`NOT_STARTED`. B189-3 is `BLOCKED` at the missing post-grant execution
boundary, not at ACK/Selection. The build/selector evidence is in
[B189 build evidence](evidence/b189-build-20260918.md); the durable audit and
r21 logs are in [static audit](evidence/spec189-static-audit-20260918.md) and
[execution evidence](evidence/b189-execution.md). No task is complete.
The host dependency closure is now enforced for all Waf-discovered direct
libraries and linker flags; stale NAC-ABE pkg-config metadata was repaired in
`/usr/local`, and the host/container `$ORIGIN` configure gates were exercised.
The malformed global NDNSD include flag was also repaired and a fresh host
configure completed with a clean cache.
The affected provider/oracle targets were rebuilt with the repaired global
closure. These checks do not advance T001–T010 status or establish MiniNDN
runtime completion.
The two Python binding setup entry points now enforce the same global external
dependency roots, with `/opt/ndnsf-stage` accepted only under the explicit
`NDNSF_CONTAINER_BUILD=1` Tiger build marker. The v2 frozen scope passed the
official read-only review-agent (`STATIC_PASS`); binding/template/setup checks
were 54 passed in the combined focused run. No real Python build or container
loader run was observed, so that review remained configuration evidence only.
Since then the validated r3 tree installed Core/DI into `/usr/local`, the root
`ndnsf` binding and Repo `_py_repoclient` binding were built against
`/usr/local/lib`, and both extension `ldd` closures were checked. This proves
host install/loader consistency only; it does not advance any task or establish
container/runtime qualification.
The installed-global SVS fallback was reviewed as a frozen Changed gate and
returned `STATIC_PASS`; the maintained build helper then generated a fresh
`spec180-native-build.json` using the host `/usr`/`/usr/local`/ONNX Runtime
closure. `LocalExperiment.py check` returned `PASS` for the complete candidate
tuple. A first prepared run (r22) was rejected before startup because its
profile listed three nodes for a two-stage manifest. The corrected r23 run
started the real topology and reached signed ACK/Selection plus protected-grant
verification on both Providers, then failed at the requester stream with
`NATIVE_STREAM_FAILED` / `stream event gap exceeded retry budget`. MiniNDN
startup and cleanup passed, but no post-grant execution marker was observed.
The 1.5 GiB temporary wire file was removed after process exit; raw logs and
manifests remain in the run directory. B189-3 stays `BLOCKED_FOR_NATIVE_EXECUTION`
and no task advances to complete. The next native attempt must first use
provider-side timing/error evidence and the C++ post-grant regression; it may
not treat the stream gap as the root cause or run the final qualification
repeat.

## Task checklist

- [ ] T001 [US1] Freeze the Qwen candidate tuple, production caller map and C++ source/build closure in `evidence/b189-convergence.md`.
- [ ] T002 [US1] Prepare the pinned Qwen graph, external initializer and two layer packages through the production preparation fixture and C++ `DI_NativeArtifactAuthority` oracle.
- [ ] T003 [US1] Connect prepare-time Repo manifest/layer publication and reusable hot lookup, with C++ commit/source-release counters in `evidence/b189-prepare.md`.
- [ ] T004 [US2] Project `PreparedModel::request` to a payload-free reference envelope and add C++ wire/oversized-model negatives.
- [ ] T005 [US2] Run the real Core ACK and two-provider Selection path and add C++ placement/no-fetch-before-Selection assertions.
- [ ] T006 [US3] Bind Provider fetch and `NativeCanonicalOnnxAssembler` to selected layer ranges and add digest/range/runner ownership cases.
- [ ] T007 [US3] Exercise the production hidden-state handoff and terminal output/failure oracle with two native providers.
- [ ] T008 [US4] Add C++ lease/runner drain counters and the bounded Python host resource sampler with explicit stop classification.
- [ ] T009 [US5] Launch the complete two-provider MiniNDN candidate through the maintained runner and preserve raw run evidence.
- [ ] T010 [US5] Repeat with a new run id, verify the immutable tuple and classify `QWEN_TWO_PROVIDER_PASS` or the first boundary.

## T001 — Freeze candidate and production path

**Goal**: identify the actual `prepare → request → ACK → Selection → provider → terminal` callers before editing.

**Read**: `docs/failure-log.md`, `docs/architecture-reading-guide.md`, `spec.md`, `plan.md`, Spec185/188 preparation and provider contracts, current Qwen scripts and Waf target definitions.

**Write**: `evidence/b189-convergence.md` and the candidate manifest template under `.codex-tmp` only.

**Steps**:

1. Use CodeGraph for `Runtime::prepare`, `PreparedModel::request`, `NativeRequestEnvelope`, ACK/Selection handlers, `NativeCanonicalOnnxAssembler`, `NativeOnnxAssemblyWorker`, provider execution and result callbacks.
2. Build a symbol definition map (`rg`/CodeGraph → translation unit → Waf target) and verify with `nm -C`/`readelf`.
3. Freeze model snapshot, split, profile, ABI closure, build tree, topology, resource floors and evidence markers.
4. Identify the real hidden-state handoff. If absent, record `PROTOCOL_BOUNDARY` as a design gap before implementation.
5. Before any new MiniNDN run, statically/preflight-check the service-scoped
   role policy, operator registry/key closure, dynamic KV tensor contract,
   graph-size planning budget, privileged Python module closure, run-scoped
   publication directory and candidate-derived digests.

**Acceptance**: immutable candidate tuple and five-lane coverage matrix are complete; official review-agent returns `STATIC_PASS`; the Spec189 C++ oracle has a registered source/definition/link map in `tests/wscript` or `examples/wscript`; no implementation claim.

## T002 — Prepare Qwen graph and split packages

**Goal**: make the real Qwen snapshot a prepare-time, two-stage candidate.

**Write**: maintained exporter/fixture changes only where production prepare requires them; C++ fixture/oracle and `evidence/b189-prepare.md`.

**Steps**:

1. Use the pinned snapshot and verify tokenizer/config/revision hashes.
2. Produce canonical graph with external initializer and layer packages for ranges `0..14` and `14..28`; preserve tensor contracts and graph identity.
3. Register the fixture through the production preparation path; do not let tests create a parallel model serializer.
4. Add C++ digest/range/config mismatch and staging-failure cases.

**Acceptance**: `DI_NativeArtifactAuthority` sees the same production manifest/reference that the later requester will use; stage export alone is not sufficient.

## T003 — Publish/reuse layer references in Repo

**Goal**: make Repo publication part of `prepare` and reusable by later requests.

**Write**: `Runtime`/Repo adapter only in the registered production path, C++ selector fixture and `evidence/b189-prepare.md`.

**Steps**:

1. Connect prepare to Repo manifest/layer commit and typed reference return.
2. Ensure a second prepare is a manifest hit and does not republish canonical source/initializer.
3. Release transient source ownership after the receipt/reference is committed; keep active lease ownership explicit.
4. Add no-READY-on-failure, duplicate identity and source-release counters.

**Acceptance**: one cold commit, one hot lookup, correct digests/sizes/ranges, no staging residue; C++ selector and build/source registration pass.

## T004 — Project payload-free request

**Goal**: use the prepared handle repeatedly without carrying model bytes.

**Write**: `PreparedModel`/`NativeRequestEnvelope` production code, C++ wire oracle, `evidence/b189-placement.md`.

**Steps**:

1. Trace the public request caller and preserve model reference, manifest digest, epoch and input reference.
2. Reject initializer bytes, canonical source bytes and arbitrary model URLs in the request path.
3. Run two requests from the same prepared handle and assert Repo publication counters do not increase.
4. Add released-handle, stale-manifest and oversized-inline negative cases.

**Acceptance**: C++ parser/oracle proves request reference-only; no Python-only assertion.

## T005 — Produce real two-provider ACK/Selection

**Goal**: bind the request to exactly two providers and exact ranges.

**Write**: Core/provider offer wiring only if required, C++ Selection oracle and `evidence/b189-placement.md`.

**Steps**:

1. Use two real provider nodes and current controller/authority path.
2. Require signed ACK followed by Selection containing provider identity, stage/range, manifest digest, epoch and attempt id.
3. Assert no layer fetch or runner creation before Selection.
4. Add no-offer, stale epoch, range overlap, unselected-provider and digest mismatch cases.

**Acceptance**: C++ oracle observes valid two-placement Selection and zero pre-Selection heavy effects.

## T006 — Fetch and assemble selected layers

**Goal**: each Provider reads only its assigned package and creates a native runner from it.

**Write**: `NativeCanonicalOnnxAssembler`, `NativeOnnxAssemblyWorker`, provider adapter/fixture and `evidence/b189-execution.md`.

**Steps**:

1. Bind provider fetch to Selection placement and Repo layer reference.
2. Verify digest, role, range, manifest and epoch before ORT load.
3. Count fetches, assembly starts, runner creation and release for both providers.
4. Add wrong-package, duplicate-range, cancellation and assembly-failure cases with drain barriers.
5. Add a C++ regression that compares every selected V3 dependency edge's
   endpoint digest with the edge used by `NativeEpochCoordinator`; legacy plan
   reconstruction must not erase the authenticated endpoint.
6. Register the regression and its source closure in the Waf target; an
   installable production binary alone is not an oracle.

**Acceptance**: `di-native-provider` and assembly worker run against selected packages, never a hidden whole-model fixture.

## T007 — Execute hidden-state handoff and terminal oracle

**Goal**: exercise the real two-stage execution and return a named result.

**Write**: production handoff/wire code if missing, C++ E2E selector/oracle and `evidence/b189-execution.md`.

**Steps**:

1. Identify and use the production Core/NDN handoff for Provider-0 output to Provider-1 input.
2. Bind hidden state to attempt id, model/manifest digest, stage and sequence; reject stale/mismatched handoff.
3. Run one deterministic input and compute a C++ output digest/top-token oracle.
4. Exercise terminal success, handoff mismatch, provider stop and Core cancellation.
5. Emit and assert the ordered provider markers
   `GRANT_VERIFIED`, `EXECUTION_ENTERED`, `DEPENDENCY_FETCH`,
   `ASSEMBLY_STARTED`, `RUNNER_READY`, `EXECUTION_COMPLETED` and `TERMINAL`.
   A generic requester stream gap is not a first-failure classification until
   the first missing provider marker is identified.

**Acceptance**: both Providers execute, a terminal response arrives, and C++ evidence proves the path; if no production handoff exists, keep `PROTOCOL_BOUNDARY` and do not shortcut.

## T008 — Measure resource and drain ownership

**Goal**: prove or classify the 12-GB host resource boundary.

**Write**: C++ lifecycle counters/selector and maintained Python sampler changes, `evidence/b189-resource.md`.

**Steps**:

1. Sample RSS, MemAvailable, swap, disk free, Repo resident bytes, materialization bytes and child states at lifecycle markers.
2. Set explicit memory/disk floors and deterministic process-group stop behavior.
3. Verify active leases/runners/callbacks and temporary windows reach baseline after success or stop.
4. Preserve raw samples for both normal and resource-stop cases.
5. The maintained MiniNDN runner must implement the sampler and guard; a
   `finally` block that only sends SIGINT/kill does not satisfy this task.

**Acceptance**: C++ ownership oracle and sampler agree; safety stop is `RESOURCE_BOUNDARY`, not PASS.

## T009 — Run the real two-provider MiniNDN candidate

**Goal**: launch the complete chain through the maintained MiniNDN script.

**Write**: `Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py` only for real-path orchestration/markers, C++ target registration, `.codex-tmp` raw run and `evidence/b189-execution.md`.

**Steps**:

1. Build affected native targets with matching ABI/source closure; verify hashes and `ldd`.
2. Derive binary/model/topology digests from the frozen candidate record; do
   not hand-copy a second digest spelling. Verify policy/credential/module
   closure and run-scoped disk residue before starting MiniNDN.
3. Run as root with `/usr/sbin:/sbin`, exactly two providers, a unique run id and resource guard.
4. Require event order `PREPARE→REPO_COMMIT→REQUEST_REFERENCE_ONLY→ACK→SELECTION→GRANT_VERIFIED→EXECUTION_ENTERED→FETCH/ASSEMBLE/EXECUTE×2→TERMINAL→DRAINED`.
5. Record the first provider-side missing marker and child exits; no upload/SIF/Tiger.

**Acceptance**: complete event sequence or durable classified boundary; a harness start is not a native PASS. The r01-r21 exploratory runs are retained as evidence but do not satisfy the dependency on T008; no new qualification run may start until the C++ oracle and resource guard are registered and reviewed.

## T010 — Repeat, converge and classify verdict

**Goal**: make the result reproducible and reviewable.

**Write**: evidence checker, `evidence/b189-convergence.md`, task status and `docs/failure-log.md` entry when blocked.

**Steps**:

1. Freeze first run evidence and compare every candidate tuple field.
2. Repeat with a new run id without changing model/profile/build/selector identity.
3. Re-run affected static review for any repair, then run the named C++ selector and MiniNDN repeat.
4. Assign only `QWEN_TWO_PROVIDER_PASS`, `PARTIAL`, `RESOURCE_BOUNDARY`, `PROTOCOL_BOUNDARY`, `BLOCKED` or `UNQUALIFIED`.

**Acceptance**: both runs agree on full chain, terminal oracle and cleanup for PASS; otherwise first boundary and next gate are explicit.

## Dependencies and strategy

`T001 → T002 → T003 → T004 → T005 → T006 → T007 → T008 → T009 → T010` is the required order. Do not parallelize tasks that change the same production caller, wire state, manifest schema or source closure. The first implementation slice is prepare/Repo reuse; the second is reference/placement; the third is provider execution/handoff; the fourth is resource/evidence. SIF/Tiger is outside this graph.

## Batch result record

Each batch evidence file must include the five lanes, review-agent snapshot/base/diff, static findings, compile/link misses, runtime/test misses, unobserved gaps, build target/`-j`/elapsed, dynamic gate card, first failure boundary and closure decision. Missing evidence keeps the affected task `PARTIAL`.
