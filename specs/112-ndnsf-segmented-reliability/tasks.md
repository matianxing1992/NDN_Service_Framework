# Tasks: NDNSF External Bug Report Corrections

**Input**: Design documents from `specs/112-ndnsf-segmented-reliability/`

**Tests**: Mandatory and test-first because this feature corrects reliability,
security, timeout, and process-lifetime defects.

**Scope rule**: Every task must trace to one of Peter's five email defects or to
the minimum build/evidence work needed to prove that defect. No public checked
publication API, large-object redesign, new status protocol, 5% loss, DI,
Docker, iTiger, or Spec 111 task is allowed.

## Phase 1: Setup And Candidate Identity

**Purpose**: Bind old-report lineage and current dirty sources without reverting
unrelated user work.

- [x] T001 Record the reporter environment/commits and current NDNSF, ndn-svs, NAC-ABE, ndn-cxx, Boost, compiler, binary, and dirty-diff identities in `specs/112-ndnsf-segmented-reliability/evidence/pre-fix-code-reality.md`
- [x] T002 Implement an atomic `create --result-root <path>` CLI that writes one immutable manifest, rejects changed/existing identity collisions, and prints only its candidate ID in `Experiments/spec112_candidate_manifest.py`
- [x] T003 [P] Add candidate manifest, dirty-digest, binary-hash, and candidate-change tests in `tests/python/test_spec112_candidate_manifest.py`
- [x] T004 Finish argument validation, exact payload generation/comparison, request identity, and process exit reporting in `examples/python/segmented_response_provider.py` and `examples/python/segmented_response_user.py`
- [x] T005 [P] Add CLI/result-schema tests for the segmented response Provider/User tools in `tests/python/test_spec112_segmented_response_tools.py`
- [x] T006 Initialize requirement-to-task-to-test mapping with all evidence labeled proposed or implemented-only in `specs/112-ndnsf-segmented-reliability/traceability.md`

---

## Phase 2: Foundational Build And Pre-Fix Evidence

**Purpose**: Establish a build-matched current baseline before changing behavior.

**Critical**: T007-T012 complete before any source fix. Each pre-fix candidate/cell
runs once; continue after a fix with a new candidate, never by replacing evidence.

- [x] T007 Restore the project's Boost 1.71 local configure gate and record why the upstream-only 1.74 rule is not the local test baseline in `../ndn-svs/wscript` and `specs/112-ndnsf-segmented-reliability/evidence/pre-fix-build.md`
- [x] T008 Reconfigure and rebuild current ndn-svs and its tests, recording exact source/binary/compiler/dependency hashes in `specs/112-ndnsf-segmented-reliability/evidence/pre-fix-build.md`
- [x] T009 Run the rebuilt existing `TestSVSPubSub` suite once and preserve all outcomes in `specs/112-ndnsf-segmented-reliability/evidence/pre-fix-unit.md`
- [x] T010 Implement required `--candidate-manifest`, exclusive MiniNDN ownership, forced/recorded `NDNSF_DISABLE_RESPONSE_LARGE_DATA_REFERENCE=1`, no-reference verification, run-once directories, wall-clock/disk stops, Provider epoch capture, repeated-size sequences, a `degraded-provider-after-targeted-bootstrap` fault profile, and JSON/CSV summaries in `Experiments/NDNSF_Segmented_Response_Minindn.py`
- [x] T011 [P] Add ownership, candidate, forced-environment, immutable-directory, repeated payload-sequence, degraded-Provider fault-profile, and summary-schema tests in `tests/python/test_spec112_segmented_response_campaign.py`
- [x] T012 Run the four pre-fix 0% cells—Normal/Targeted × synchronous/asynchronous SVS publication—with `64,4000,5000,6500,8000,16000` once and record immutable result paths in `specs/112-ndnsf-segmented-reliability/evidence/pre-fix-minindn.md`

**Checkpoint**: Current source and actual rebuilt binaries have admissible
pre-fix evidence. Expected failures remain visible.

---

## Phase 3: User Story 1 - Segmented Responses And Oversize Abort (Priority: P1) MVP

**Goal**: Close email defects 1 and 2 without changing public publication APIs or
the existing automatic large-response reference behavior.

**Independent Test**: Forced inline-SVS 6.5/8/16-KB responses complete byte-exactly;
80×8-KB is followed in the same Provider epoch by 10×64-B and 12×4-KB successes.

### Tests For User Story 1

- [x] T013 [US1] Add final signed-inner/signed-outer 8800-B exact/below/above boundary, exact-content-multiple/no-empty-tail, and short/long name/certificate tests in `../ndn-svs/tests/unit-tests/svspubsub.t.cpp`
- [x] T014 [US1] Add preparation/reservation/storage/advertisement ordering and injected encode/sign/store/`Face::put` failure tests that assert no consumed visible sequence gap, no later-sequence advance past failure, no false advertisement, and continued small publication health in `../ndn-svs/tests/unit-tests/svspubsub.t.cpp`
- [x] T015 [US1] Add validation-failure, missing/malformed/duplicate segment, timeout, shutdown, callback-lifetime, and bounded cleanup tests in `../ndn-svs/tests/unit-tests/svspubsub.t.cpp`
- [x] T016 [P] [US1] Add compiled-binding normal/Targeted byte-exact response tests for 64 B through 16 KB under both SVS publish modes in `tests/python/test_spec112_segmented_response.py`

### Implementation For User Story 1

- [x] T017 [US1] Replace fixed raw-content segmentation with iterative final signed-inner/signed-outer wire-size fitting while preserving existing public signatures in `../ndn-svs/ndn-svs/svspubsub.cpp` and `../ndn-svs/ndn-svs/svspubsub.hpp`
- [x] T018 [US1] Move fallible preparation before irreversible asynchronous sequence exposure, commit or roll back tentative reservations under one ordering owner, prepare/store the complete logical publication before advertisement, and discard incomplete state on failure in `../ndn-svs/ndn-svs/svspubsub.cpp` and `../ndn-svs/ndn-svs/svsync-base.cpp`
- [x] T019 [US1] Contain encoding/signing/`Face::put` exceptions on the owning event-loop boundary and make validation success/failure callbacks lifetime-safe with deterministic cleanup in `../ndn-svs/ndn-svs/svspubsub.cpp` and `../ndn-svs/ndn-svs/svsync-base.cpp`
- [x] T020 [US1] Rebuild/install matched ndn-svs, run T013-T016 to completion, and record source/binary hashes and results in `specs/112-ndnsf-segmented-reliability/evidence/us1-focused-validation.md`
**Checkpoint**: Email defects 1 and 2 pass focused unit/compiled-binding tests.
Their final MiniNDN proof is deferred until all necessary source changes are
sealed into one integrated candidate.

---

## Phase 4: User Story 2 - Current Python Targeted Invocation (Priority: P1)

**Goal**: Close email defect 3 by proving or minimally correcting the real current
binding without exposing tokens-off operation.

**Independent Test**: One compiled Python registration completes normal and
Targeted calls with tokens enabled; invalid/replayed tokens invoke no handler.

### Tests For User Story 2

- [x] T021 [US2] Add compiled-binding normal plus Targeted bootstrap/fast-path tests with one registered handler and exactly-once invocation in `tests/python/test_ndnsf_targeted_python_api.py`
- [x] T022 [US2] Add missing/mismatched/consumed/replayed token and no-public-token-disable negative tests in `tests/python/test_ndnsf_targeted_python_api.py` and `tests/unit-tests/generic-dynamic-api-targeted.t.cpp`

### Conditional Implementation For User Story 2

- [x] T023 [US2] Execute the current-binding decision gate: record a no-source-change disposition when T021-T022 pass, otherwise minimally correct tokens-on Provider/User setup and `NormalAndTargeted` registration without adding a tokens-off API in `pythonWrapper/src/ndnsf/_ndnsf.cpp`, `pythonWrapper/ndnsf/service.py`, and `specs/112-ndnsf-segmented-reliability/evidence/us2-python-targeted.md`
- [x] T024 [US2] Run real-binding positive/negative tests and record exact outcomes and binding hashes in `specs/112-ndnsf-segmented-reliability/evidence/us2-python-targeted.md`

**Checkpoint**: Email defect 3 is proven fixed in the current version or repaired
minimally; the old fork is not blindly cherry-picked.

---

## Phase 5: User Story 3 - Targeted timeout_ms (Priority: P1)

**Goal**: Close email defect 4 with one total deadline and exactly one existing
response-or-timeout callback.

**Independent Test**: An established Targeted Provider becomes unavailable;
the next request calls `on_timeout` once by `timeout_ms + 500 ms`.

### Tests For User Story 3

- [x] T025 [US3] Add tests proving the deadline starts before admission/bootstrap/publication and still fires after publication exception or non-completion in `tests/unit-tests/generic-dynamic-api-targeted.t.cpp`
- [x] T026 [US3] Add response-versus-timeout, late-response, duplicate-event, exactly-once callback, and request-state cleanup tests in `tests/unit-tests/generic-dynamic-api-targeted.t.cpp`
- [x] T027 [P] [US3] Add compiled Python async callback and bounded sync-adapter tests against absent/degraded Providers in `tests/python/test_spec112_targeted_timeout.py`

### Implementation For User Story 3

- [x] T028 [US3] Create/store/schedule the absolute Targeted deadline immediately after pending request creation, before admission/bootstrap/publication, in `ndn-service-framework/ServiceUser.cpp` and `ndn-service-framework/ServiceUser.hpp`
- [x] T029 [US3] Arbitrate response and timeout through one terminal transition, preserve the deadline across publication errors, and reclaim timer/request/token state exactly once in `ndn-service-framework/ServiceUser.cpp` and `ndn-service-framework/ServiceUser.hpp`
- [x] T030 [US3] Bound the Python synchronous adapter by the native deadline and preserve one asynchronous response-or-timeout callback in `pythonWrapper/src/ndnsf/_ndnsf.cpp` and `pythonWrapper/ndnsf/service.py`
- [x] T031 [US3] Run T025-T027 and record callback counts, deadline error, source hashes, and focused outcomes in `specs/112-ndnsf-segmented-reliability/evidence/us3-targeted-timeout.md`

**Checkpoint**: Email defect 4 is closed independently of large responses.

---

## Phase 6: User Story 4 - NAC-ABE/OpenABE Exit (Priority: P1)

**Goal**: Close email defect 5 with executed initialized-process evidence and no
speculative teardown rewrite.

**Independent Test**: 100 initialized cycles, each containing Controller,
Provider, and User (300 role exits total), finish without SIGSEGV or SIGABRT.

### Tests For User Story 4

- [x] T032 [US4] Extend the NAC-ABE unit fixture to initialize/use cryptography and report normal/control-shutdown exit code, signal, timeout, and sanitizer result in `../NAC-ABE/tests/unit-tests/abe-support.t.cpp`
- [x] T033 [P] [US4] Add a 100-cycle subprocess driver that runs one initialized Controller, Provider, and User per cycle (300 role exits total) plus result-schema tests in `tests/python/test_spec112_nac_abe_exit.py`
- [x] T034 [US4] Run the current code lifecycle suite before teardown edits and preserve all outcomes in `specs/112-ndnsf-segmented-reliability/evidence/us4-pre-fix-lifecycle.md`

### Conditional Implementation For User Story 4

- [x] T035 [US4] Execute the lifecycle decision gate: record a no-source-change disposition when T032-T034 pass, otherwise minimally enforce process-wide same-thread OpenABE lifetime and prevent unsafe destructor-time `ShutdownOpenABE` in `../NAC-ABE/src/algo/abe-support.cpp`, `../NAC-ABE/src/algo/abe-support.hpp`, and `specs/112-ndnsf-segmented-reliability/evidence/us4-openabe-exit.md`
- [x] T036 [US4] Run 100 initialized Controller/Provider/User cycles (300 role exits total) on the unchanged passing code or a new fixed candidate and record zero-crash counts, hashes, and sanitizer limits in `specs/112-ndnsf-segmented-reliability/evidence/us4-openabe-exit.md`

**Checkpoint**: Email defect 5 has executed evidence; a source change exists only
when a current crash required it.

---

## Phase 7: Integrated Candidate, MiniNDN Evidence And Closeout

- [x] T037 Run the complete rebuilt ndn-svs, focused NDNSF C++, compiled Python Targeted/timeout, and NAC-ABE lifecycle suites and record exact commands/hashes in `specs/112-ndnsf-segmented-reliability/evidence/final-focused-validation.md`
- [x] T038 Freeze one integrated final candidate only after T037 passes, rejecting any later source/binary/configuration mutation in `results/spec112-segmented/<candidate>/candidate-manifest.json`
- [x] T039 Run the four final 0% forced-SVS boundary cells—Normal/Targeted × synchronous/asynchronous SVS publication—exactly once in `results/spec112-segmented/<candidate>/campaign-cells.csv`
- [x] T040 Run one final-candidate 80×8-KB burst followed without Provider restart by 10×64-B and 12×4-KB checks in `results/spec112-segmented/<candidate>/campaign-cells.csv`
- [x] T041 Run one final-candidate 0% degraded-Provider Targeted timeout cell exactly once and record callback counts and deadline error in `results/spec112-segmented/<candidate>/campaign-cells.csv`
- [x] T042 Validate final summaries for byte equality, 8800-B limit, Provider epoch/liveness, callback counts, no-reference proof, exits, ownership, candidate identity, and immutable paths in `results/spec112-segmented/<candidate>/campaign-summary.json`
- [x] T043 Write a five-defect disposition table—reproduced, root cause, changed or already fixed, focused test, MiniNDN/lifecycle evidence, residual limitation—in `specs/112-ndnsf-segmented-reliability/completion-summary.md`
- [x] T044 Update exact result paths, acceptance counts, and evidence levels for every FR/SC without adding new mechanisms in `specs/112-ndnsf-segmented-reliability/traceability.md`
- [x] T045 Run the strict structure scan, prerequisite check, `git diff --check`, focused scope search, and code-aware pre-completion audit; record PASS or blockers in `specs/112-ndnsf-segmented-reliability/evidence/pre-completion-audit.md`
- [x] T046 Run Spec Kit convergence and append only genuinely unbuilt work tied to the five defects in `specs/112-ndnsf-segmented-reliability/tasks.md`
- [x] T047 After all convergence tasks are complete, rerun strict structure, semantic, code-aware, evidence, and scope audits and record the final verdict in `specs/112-ndnsf-segmented-reliability/evidence/final-audit.md`

## Dependencies And Execution Order

```text
Setup T001-T006
  -> Build/pre-fix T007-T012
      -> US1 T013-T020
      -> US2 T021-T024
      -> US3 T025-T031
      -> US4 T032-T036
          -> Integrated closeout T037-T047
```

- US2, US3, and US4 can proceed independently after Phase 2.
- US1 changes adjacent ndn-svs and must rebuild/install before the integrated
  candidate's final MiniNDN cells.
- T023 and T035 are decision gates: a passing current-code regression explicitly
  requires the no-source-change branch and forbids speculative source edits.
- Final evidence uses one integrated candidate after all necessary source changes.

## Parallel Opportunities

- T002/T003 and T004/T005 can proceed on separate files after T001.
- T011 can be written while T007-T009 restore the build.
- T016 is independent of the ndn-svs unit test file edited by T013-T015.
- After Phase 2, US2 test authoring, US3 test authoring, and US4 harness authoring
  touch separate files.
- T027 and T033 are independent Python test modules.

## Implementation Strategy

1. Establish the current rebuilt pre-fix result once.
2. Fix and validate defects 1/2 as the MVP because they can kill/degrade Provider.
3. Verify before modifying defects 3 and 5, which may already be corrected.
4. Fix defect 4 independently using small payloads.
5. Freeze one integrated candidate, run final declared evidence once, audit scope,
   then converge.

After each failing test is fixed, continue the current focused sequence rather
than restarting the entire suite. Run the complete focused suite once after all
individual tests have been exercised.

## Phase 8: Convergence

- [x] T048 [US3] Replace the synchronous Python Targeted adapter's stack-reference callback captures with copied submission inputs and a shared exactly-once terminal state; add a regression against late completion after the local fallback and rerun the focused binding/timeout tests in `pythonWrapper/src/ndnsf/_ndnsf.cpp` and `tests/python/test_spec112_targeted_timeout.py` per FR-013 (partial)
- [x] T049 [US3] Rebuild the Python extension after T048, rerun the focused C++ Targeted and Python binding suites, and freeze a new immutable integrated candidate whose manifest binds the rebuilt extension in `results/spec112-segmented/<candidate>/candidate-manifest.json` per FR-017 (partial)
- [x] T050 Run the six declared final MiniNDN cells exactly once under the T049 candidate, validate all FR/SC acceptance counts, and update `completion-summary.md`, `traceability.md`, and focused/final evidence paths without overwriting the prior candidate per FR-018 and SC-008 (partial)
