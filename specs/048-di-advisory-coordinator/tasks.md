# Tasks: NDNSF-DI Advisory Coordinator

## Phase 1: Design Artifacts

- [x] T001 Define the coordinator boundary: optional advisory role, user-side
  planner default, provider lease authority preserved.
- [x] T002 Record requirements, non-goals, validation plan, and future MiniNDN
  integration boundary in Spec Kit artifacts.
- [x] T003 Perform a second-pass checklist while retaining maintainer
  responsibility for final architecture and verification.

## Phase 2: Runtime Contract

- [x] T004 Add `CoordinatorMode`, `PlanIntent`,
  `AdvisoryCoordinatorConfig`, `CoordinatorWindow`, and
  `AdvisorySuggestion`.
- [x] T005 Add deterministic advisory proof helpers for MVP regression
  coverage.
- [x] T006 Add fairness-aware advisory assignment selection using existing
  runtime-aware candidate scoring.
- [x] T007 Add `AdvisoryCoordinator.suggest(...)`.
- [x] T008 Add `merge_advisory_suggestion(...)` so users accept suggestions
  only after local freshness/proof/context/candidate/lease checks.
- [x] T009 Export the new API from the Python package.

## Phase 3: Tests

- [x] T010 Test disabled coordinator returns no suggestions.
- [x] T011 Test two simultaneous users are balanced across equivalent
  providers.
- [x] T012 Test valid fresh suggestions can be accepted.
- [x] T013 Test stale suggestions are ignored.
- [x] T014 Test tampered proofs are ignored.
- [x] T015 Test suggestions cannot bypass current provider lease validation.

## Phase 4: Documentation and Verification

- [x] T016 Add project documentation for the advisory coordinator workflow.
- [x] T017 Link the advisory coordinator from the runtime workflow.
- [x] T018 Run focused and existing Python runtime tests.
- [x] T019 Run `git diff --check`.
- [x] T020 Record remaining future work: C++ transport service, signed proof,
  and MiniNDN coordinator-service evidence.
