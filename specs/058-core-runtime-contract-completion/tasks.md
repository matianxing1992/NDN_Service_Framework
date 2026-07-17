# Tasks: Core Runtime Contract Completion

**Input**: Design documents from `/specs/058-core-runtime-contract-completion/`

**Prerequisites**: plan.md, spec.md, research.md, data-model.md, contracts/runtime-envelope-contract.md

## Phase 1: Setup

- [x] T001 Record the feature plan and active Spec Kit pointer in `.specify/feature.json`
- [x] T002 [P] Add runtime-envelope contract documentation in `specs/058-core-runtime-contract-completion/contracts/runtime-envelope-contract.md`

---

## Phase 2: Foundational Core Contract

- [x] T003 [P] Add C++ `ServiceOperationStatus`, `DataProductReference`, and `ProviderCapabilityHint` structs in `ndn-service-framework/ServiceProvider.hpp`
- [x] T004 Add C++ encode/decode helpers for operation status and provider capability payloads in `ndn-service-framework/ServiceProvider.cpp`
- [x] T005 [P] Add Python `ServiceDiscoveryRecord` and `ServiceDiscoverySnapshot` helpers in `pythonWrapper/ndnsf/service_discovery.py`
- [x] T006 Export Python service discovery helpers from `pythonWrapper/ndnsf/__init__.py`

---

## Phase 3: User Story 1 - Core Runtime Envelopes Are Reusable Across C++ and Python (Priority: P1)

**Independent Test**: C++ and Python core tests can build and parse the same concepts without importing app-specific packages.

- [x] T007 [US1] Add C++ provider capability and operation status round-trip tests in `tests/unit-tests/generic-admission-lease.t.cpp`
- [x] T008 [US1] Add Python discovery and drain classification tests in `tests/python/test_ndnsf_core_service_discovery.py`
- [x] T009 [US1] Run focused core C++/Python tests listed in `quickstart.md`

---

## Phase 4: User Story 2 - Apps Keep Domain Semantics But Emit Core-First Bridges (Priority: P2)

**Independent Test**: Existing Repo/UAV/DI migration tests prove app payloads remain app-owned while core envelopes are preferred.

- [x] T010 [US2] Update `docs/ndnsf-core-app-boundary.md` with C++ envelope and discovery/drain completion guidance
- [x] T011 [US2] Verify DI dependency docs/comments keep exact-name large-data as default and stream chunk wrapping diagnostic-only in `NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp`
- [x] T012 [US2] Run `tests/python/test_ndnsf_app_core_envelope_migration.py`

---

## Phase 5: User Story 3 - Service Discovery, Drain, and Provider-Pair Telemetry Are Core Surfaces (Priority: P3)

**Independent Test**: Service discovery snapshot tests classify provider states and preserve runtime/telemetry payloads.

- [x] T013 [US3] Add provider drain state constants and ready-for-new-request helper in `pythonWrapper/ndnsf/service_discovery.py`
- [x] T014 [US3] Verify service discovery helper can consume `ProviderCapabilityHint`, `NdnsdProviderState`, and raw dict inputs in `tests/python/test_ndnsf_core_service_discovery.py`
- [x] T015 [US3] Document provider-pair telemetry as a core fact source in `docs/ndnsf-core-app-boundary.md`

---

## Phase 6: Polish & Validation

- [x] T016 Run `git diff --check`
- [x] T017 Run focused Python regression suite for boundary and app migration tests
- [x] T018 Mark all completed tasks in `specs/058-core-runtime-contract-completion/tasks.md`

## Dependencies & Execution Order

- Phase 1 before all implementation.
- Phase 2 before user stories.
- US1 before US2/US3 validation because it introduces helpers.
- US2 and US3 can proceed after Phase 2.
- Phase 6 after all story tasks.

## Implementation Strategy

Deliver the smallest safe core increment first: typed helper structs and tests.
Then add discovery/drain classification and documentation. Avoid changing Repo,
UAV, or DI domain policy.
