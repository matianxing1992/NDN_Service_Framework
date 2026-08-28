# Tasks: Submission-Ready NDNSF Evidence

**Input**: Design documents from `specs/173-paper-submission-evidence/`

**Prerequisites**: `plan.md`, `spec.md`, `research.md`, `data-model.md`, `contracts/`, `quickstart.md`

**Tests**: Campaign and analysis behavior is implemented test-first. Network claims require MiniNDN; smoke/pilot output is never admitted as confirmatory evidence.

**Organization**: Tasks are grouped by user story and each task closes one reviewable evidence outcome.

## Format: `[ID] [P?] [Story] Description`

- **[P]**: Can proceed independently after its phase prerequisites
- **[Story]**: Maps to a user story in `spec.md`
- Every task includes its source, test, evidence, or manuscript path

---

## Phase 1: Setup (Shared Infrastructure)

**Purpose**: Establish the frozen runtime and artifact inputs before measuring anything

- [x] T001 Reconfigure and rebuild the complete current C++ targets with NDN-SVS Experimental, ndn-cxx, and Boost 1.71; require `App_User`, `App_Provider`, `App_ServiceController`, unit tests, and integration tests to exist; then retain exact Git revisions plus reproducible dirty-diff snapshots/hashes and record compiler, Python, MiniNDN, disk, executable/library hashes, and `ldd` resolution in `specs/173-paper-submission-evidence/evidence/toolchain/` and `specs/173-paper-submission-evidence/evidence/toolchain-manifest.json`, stopping before experiments if any binary resolves outside the intended system toolchain

---

## Phase 2: Foundational (Blocking Prerequisites)

**Purpose**: Make the frozen matrix executable and its output deterministically auditable

**⚠️ CRITICAL**: No confirmatory result can be admitted before these behaviors pass on fixtures and dry-run plans.

- [x] T002 [P] Implement a test-first, registration-driven matched campaign runner with deterministic system ordering, exact command/revision manifests, preflight, pilot/confirmatory separation, hash-verified `--resume`, infrastructure invalidation, matched-block rerun instructions, immutable attempt directories, and no outcome-based retries in `Experiments/paper_submission_campaign.py` and `tests/python/test_spec173_paper_submission_campaign.py`
- [x] T003 [P] Implement a test-first evidence normalizer/auditor that reconciles scheduled/issued/admitted/successful/timed-out NDNSF, gRPC, and NSC outcomes, applies mechanism-aware load validity, aggregates independent repetitions without packet pseudoreplication, retains exclusions and hashes, validates `contracts/artifact-index.schema.json`, and rejects unsupported manuscript precision in `scripts/analyze_paper_submission_evidence.py` and `tests/python/test_spec173_paper_submission_evidence.py`

**Checkpoint**: A dry-run expands every registered cell without starting MiniNDN, and fixture analysis produces the same canonical summary and hashes on repeated execution.

---

## Phase 3: User Story 1 - Defensible Quantitative Claims (Priority: P1) 🎯 MVP

**Goal**: Every retained quantitative item is either backed by a canonical package or removed/replaced.

**Independent Test**: Audit all numerical manuscript items against `evidence/artifact-index.json`; no supported item may depend on chat history, a missing directory, or a commit message alone.

### Behavioral Tasks for User Story 1

- [x] T004 [P] [US1] Recover and normalize the retained 1/3/10-percent loss summaries, authorization/regression evidence, mobility holdout, and work-efficiency evidence; record exact source revision/path/hash and mark the missing one-Provider, admission, selection-performance, and physical-machine precision as replacement or removal candidates in `specs/173-paper-submission-evidence/evidence/historical/` and `specs/173-paper-submission-evidence/evidence/artifact-index.json`
- [x] T005 [US1] Close the operational gate by running the registered MiniNDN pilot and Selective-ACK correctness regression, verifying all system summaries and invalidity rules without inspecting or promoting pilot rankings, and recording the decision to proceed or the matched configuration defect in `results/spec173-paper-submission-pilot/` and `specs/173-paper-submission-evidence/evidence/pilot-gate.md`
- [x] T006 [US1] Execute or hash-verified-resume the complete frozen confirmatory matrix with all three repetitions and matched-block failure handling, preserve negative and neutral cells, and retain exact per-run manifests/summaries/hashes in `results/spec173-paper-submission-confirmatory-v2/` without changing `contracts/experiment-registration.yaml` after result inspection
- [x] T007 [US1] Analyze and admit only valid confirmatory and recovered evidence, report every repetition plus mean/sample-SD/min/max, reconcile generated/admitted/published/completed counts, and finalize claim dispositions and reproducible tables in `specs/173-paper-submission-evidence/evidence/`

**Checkpoint**: User Story 1 passes when 100% of candidate numerical items are supported, qualified, replaced, or removed and no registered control is omitted.

---

## Phase 4: User Story 2 - Preserve and Sharpen the Scientific Story (Priority: P2)

**Goal**: Retain the May 20 scientific core while foregrounding the bounded NDNSF composition and measured advantages.

**Independent Test**: A section-by-section comparison against `Named_Data_Network_Service_Framework-20260520.pdf` finds every ledger item represented, and title/abstract/contributions/evaluation/conclusion agree on claim scope.

### Behavioral Tasks for User Story 2

- [x] T008 [US2] Revise the manuscript as one coherent claim pass: replace or remove unsupported tables, integrate admitted evidence and variation, retain every item in `contracts/core-content-ledger.md`, distinguish inherited NDN/ABE/SVS/RPC primitives from NDNSF's transaction composition, and keep stable wording for bounded Provider-discovery, authorization, mobility, and work-efficiency advantages across `docs/PAPER/named-data-network-service-framework-paper/NDNSF.tex`, `docs/PAPER/named-data-network-service-framework-paper/sections/`, and `specs/173-paper-submission-evidence/evidence/core-content-comparison.md`
- [x] T009 [US2] Produce and visually inspect the ten-body-page manuscript, resolving references, floats, clipping, and readability without deleting core content; record source/PDF hashes and rendered-page checks in `docs/PAPER/named-data-network-service-framework-paper/NDNSF.pdf` and `docs/PAPER/named-data-network-service-framework-paper/SUBMISSION_AUDIT.md`

**Checkpoint**: User Story 2 passes when the old scientific core remains visible and no section overstates what the admitted evidence shows.

---

## Phase 5: User Story 3 - Reviewer-Auditable Submission Package (Priority: P3)

**Goal**: Make every retained result navigable and reproducible without private context.

**Independent Test**: Starting only from the PDF and artifact index, an auditor can locate conditions, commands, repetitions, analysis, limitations, and hashes for each table or figure.

### Behavioral Tasks for User Story 3

- [x] T010 [US3] Build the final reviewer-facing artifact map and reproduction guide, validate every table/figure/quantitative claim bidirectionally against the manuscript and evidence packages, and document disclosed baseline differences, exclusions, venue-pending items, and compact reproduction commands in `specs/173-paper-submission-evidence/evidence/artifact-index.json`, `specs/173-paper-submission-evidence/evidence/README.md`, and `docs/PAPER/named-data-network-service-framework-paper/SUBMISSION_AUDIT.md`

**Checkpoint**: User Story 3 passes when the artifact audit succeeds from repository files alone.

---

## Phase 6: Polish & Cross-Cutting Verification

**Purpose**: Close all submission-level gates together

- [x] T011 Run the complete Spec 173 verification suite—campaign/analyzer tests, retained C++/security regressions, artifact/schema audit, clean LaTeX build, citation/reference scan, ten-body-page count, PDF render inspection, and clean claim-consistency review—and record commands, outcomes, residual limitations, and the next venue-dependent work in `specs/173-paper-submission-evidence/evidence/final-verification.md`

---

## Dependencies & Execution Order

### Phase Dependencies

- **Setup (Phase 1)**: starts immediately and freezes the runtime inputs.
- **Foundational (Phase 2)**: depends on T001 and blocks confirmatory execution.
- **User Story 1 (Phase 3)**: T004 can proceed after T001 in parallel with Phase 2; T005 requires T002/T003; T006 requires T005; T007 requires T004/T006.
- **User Story 2 (Phase 4)**: requires T007 because replacement values and removals must be final before prose/layout convergence.
- **User Story 3 (Phase 5)**: requires T008/T009 and the final US1 artifact set.
- **Polish (Phase 6)**: requires all three stories.

### User Story Dependencies

- **US1 (P1)**: evidence MVP and prerequisite for final numerical prose.
- **US2 (P2)**: depends on US1's admitted evidence but is independently testable through the core-content and claim-consistency gates.
- **US3 (P3)**: packages US1/US2 outputs and is independently testable from the artifact index.

### Parallel Opportunities

- T002 and T003 can proceed in parallel after T001 because the runner and fixture-driven analyzer have separate implementation surfaces.
- T004 can proceed alongside T002/T003 because it only reads retained archives/history and writes `evidence/historical/`.
- Within T006, systems must follow the registered deterministic block order; do not parallelize cells on one MiniNDN host.

---

## Parallel Example: Foundation and Historical Recovery

```text
Task T002: registration-driven campaign execution and manifests
Task T003: fixture-driven normalization, aggregation, and audits
Task T004: recovered historical evidence and claim dispositions
```

---

## Implementation Strategy

### MVP First (User Story 1)

1. Complete T001 and the T002/T003 foundation.
2. Recover retained evidence in T004 while the foundation is developed.
3. Pass the pilot gate in T005 before any long campaign.
4. Execute and freeze T006/T007 without outcome-dependent configuration changes.
5. Stop and audit US1 independently before editing final manuscript numbers.

### Incremental Delivery

1. **Evidence authority**: US1 eliminates unverifiable precision.
2. **Scientific narrative**: US2 preserves the May 20 core and sharpens bounded novelty.
3. **Reviewer package**: US3 exposes provenance and reproduction.
4. **Submission gate**: T011 verifies the integrated result.

## Fragmentation Scan

- Campaign unit tests, runner implementation, dry-run verification, and manifest behavior remain one task (T002).
- Analyzer unit tests, normalization, aggregation, schema validation, and claim audit remain one task (T003).
- Pilot execution and its go/no-go evidence remain one operational gate (T005).
- Manuscript claim edits and the corresponding core-content comparison remain one coherent revision task (T008).
- PDF build, page-layout correction, and rendered evidence remain one acceptance task (T009).

## Notes

- The registration is frozen before pilot ranking inspection; pilot output cannot support paper claims.
- Do not use host NFD for final evidence.
- Do not edit NDN-SVS Sync timing as part of this campaign.
- Do not retain favorable-only repetitions, rates, or systems.
- Venue-specific anonymity/disclosure/formatting remains pending until the user identifies a venue.
