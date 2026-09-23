# Tasks: UAV Operational Layer

**Input**: Design documents from `specs/069-uav-operational-layer/`

**Tests**: Required for each user story because this feature defines reusable
state contracts.

## Phase 1: Setup

- [x] T001 Create Spec069 artifacts in `specs/069-uav-operational-layer/`.
- [x] T002 Review current UAV state models and core/app boundary in
  `NDNSF-UAV-APP/shared/UavProtocol.*` and `docs/ndnsf-core-app-boundary.md`.
- [x] T003 Use CodeGraph and second-pass planning before edits.

## Phase 2: Foundational

- [x] T004 Add operational layer state declarations in
  `NDNSF-UAV-APP/shared/UavProtocol.hpp`.
- [x] T005 Add operational layer serialization, status, and validation helpers
  in `NDNSF-UAV-APP/shared/UavProtocol.cpp`.

## Phase 3: User Story 1 - Persistent Mission Operations (P1)

- [x] T006 [US1] Add `MissionPlanDocument` state and field contract in
  `NDNSF-UAV-APP/shared/UavProtocol.*`.
- [x] T007 [US1] Add mission-plan document round-trip test in
  `tests/unit-tests/uav-protocol-state.t.cpp`.

## Phase 4: User Story 2 - Operational Data Product Catalog (P2)

- [x] T008 [US2] Add `UavDataProductCatalogState` and recording bridge in
  `NDNSF-UAV-APP/shared/UavProtocol.*`.
- [x] T009 [US2] Add catalog summary round-trip test in
  `tests/unit-tests/uav-protocol-state.t.cpp`.

## Phase 5: User Story 3 - Vehicle Capability and Parameter View (P3)

- [x] T010 [US3] Add `VehicleParameterSnapshot` full and compact field views in
  `NDNSF-UAV-APP/shared/UavProtocol.*`.
- [x] T011 [US3] Add parameter/capability view tests in
  `tests/unit-tests/uav-protocol-state.t.cpp`.

## Phase 6: User Story 4 - Operator Authority Lease (P4)

- [x] T012 [US4] Add `OperatorAuthorityLease` validation helper in
  `NDNSF-UAV-APP/shared/UavProtocol.*`.
- [x] T013 [US4] Add lease allow/reject tests in
  `tests/unit-tests/uav-protocol-state.t.cpp`.

## Phase 7: Polish and Validation

- [x] T014 Add field contract and quickstart docs in
  `specs/069-uav-operational-layer/`.
- [x] T015 Update core/app boundary documentation in
  `docs/ndnsf-core-app-boundary.md`.
- [x] T016 Build `unit-tests`.
- [x] T017 Run `UavProtocolState`.
- [x] T018 Run Python app-core envelope migration regression.
- [x] T019 Run `git diff --check`.

## Phase 8: Mission Plan File Persistence

- [x] T020 [US1] Add `MissionPlanDocument` file save/load helpers in
  `NDNSF-UAV-APP/shared/UavProtocol.*`.
- [x] T021 [US1] Extend `UavProtocolState` mission-plan test with a temporary
  file save/load round-trip.
- [x] T022 Update Spec069 plan, quickstart, and field contract to document the
  file format.
- [x] T023 Re-run build, focused C++ unit test, Python envelope regression, and
  whitespace check after the file-persistence slice.

## Phase 9: Ground-Station Save/Load Wiring

- [x] T024 [US1] Add ground-station runtime methods for saving a current or
  preview mission plan and loading a saved mission plan document.
- [x] T025 [US1] Add `mission-plan-file` config/CLI support.
- [x] T026 [US1] Add GUI path entry plus `Save Plan` and `Load Plan` buttons to
  the Map / Mission workflow.
- [x] T027 [US1] Update functionality state/test expectations so mission files
  become available when a mission plan exists.
- [x] T028 Re-run build, focused C++ unit test, Python envelope regression, and
  whitespace check after ground-station wiring.

## Phase 10: Upload Loaded Mission Plan

- [x] T029 [US1] Add a ground-station runtime upload path for an existing
  `MissionPlan`, preserving per-drone mission parts.
- [x] T030 [US1] Change `Upload Mission` to prefer the current loaded or
  preview mission plan before falling back to generated patrol inputs.
- [x] T031 [US1] Update quickstart docs to explain loaded-plan upload behavior.
- [x] T032 Re-run build, focused C++ unit test, Python envelope regression, and
  whitespace check after loaded-plan upload wiring.

## Phase 11: Loaded Mission Plan Smoke Test

- [x] T033 [US1] Add a ground-station auto test that saves, loads, and uploads
  a generated `MissionPlanDocument`.
- [x] T034 [US1] Add MiniNDN harness support for
  `--auto-loaded-mission-plan-test`.
- [x] T035 [US1] Document the loaded mission plan MiniNDN smoke command.
- [x] T036 Run build, focused C++ unit test, Python envelope regression,
  whitespace check, and loaded mission MiniNDN smoke when practical.

## Phase 12: Repo-Backed Data Product Catalog Browsing

- [x] T037 [US2] Add a drone repo catalog service that summarizes the
  recording repo through `UavDataProductCatalogState`.
- [x] T038 [US2] Add ground-station runtime and GUI controls for browsing the
  selected drone's repo catalog.
- [x] T039 [US2] Add MiniNDN harness support for
  `--auto-repo-catalog-browse-test`.
- [x] T040 [US2] Extend catalog state tests so repo chunks collapse into
  object-level recording products.
- [x] T041 Document the repo catalog MiniNDN smoke command.
- [x] T042 Run build, focused C++ unit test, Python envelope regression,
  whitespace check, and repo catalog MiniNDN smoke when practical.

## Phase 13: MAVLink Parameter Cache Service

- [x] T043 [US3] Add per-drone `/UAV/MAVLink/Parameters` service suffix,
  runtime config, and controller policy entries.
- [x] T044 [US3] Add mock and UDP flight-controller parameter snapshots using
  the existing `VehicleParameterSnapshot` state contract.
- [x] T045 [US3] Add ground-station request/cache methods plus a
  vehicle-parameter inspector and `Fetch Params` GUI button.
- [x] T046 [US3] Add MiniNDN harness support for `--auto-parameter-cache-test`.
- [x] T047 [US3] Document the parameter-cache smoke command and validation
  marker.
- [x] T048 Run build, focused C++ unit test, Python envelope regression,
  whitespace check, and parameter-cache MiniNDN smoke.

## Phase 14: Operator Authority Lease Runtime Gate

- [x] T049 [US4] Add a ground-station runtime active authority lease with a
  default local `all/control` lease for existing demos.
- [x] T050 [US4] Gate direct and sync MAVLink command paths with
  `OperatorAuthorityLease::allowsCommand` before readiness, in-flight, or
  network dispatch work.
- [x] T051 [US4] Gate mission assignment entry points so monitor-only or
  expired leases fast-fail before NDNSF mission requests are published.
- [x] T052 [US4] Add a headless runtime smoke path for monitor-only and
  expired-lease rejection.
- [x] T053 [US4] Add MiniNDN harness support for
  `--auto-authority-lease-test`.
- [x] T054 Run build, focused C++ unit test, Python envelope regression,
  whitespace check, and authority-lease MiniNDN smoke.

## Phase 15: Configurable Operator Authority Lease Visibility

- [x] T055 [US4] Add ground-station startup configuration for local operator
  authority leases: operator id, covered drone, scope, and TTL.
- [x] T056 [US4] Add a ground-station inspector section that shows the active
  operator lease and whether the selected drone allows telemetry/control.
- [x] T057 [US4] Add a headless configured-lease smoke path that starts with a
  monitor lease and verifies control and mission assignment fast-fail.
- [x] T058 [US4] Add MiniNDN harness support and documentation for
  `--auto-authority-config-test`.
- [x] T059 Run build, focused C++ unit test, Python envelope regression,
  whitespace check, and configured-authority MiniNDN smoke.

## Phase 16: NDNSF Operator Authority Lease Issuer

- [x] T060 [US4] Add an app-level `/UAV/GS/OperatorAuthority/Lease`
  service name, runtime config field, and policy entries.
- [x] T061 [US4] Add a typed `OperatorAuthorityLeaseRequest` contract with
  Fields round-trip and validation coverage.
- [x] T062 [US4] Register a GS provider service that issues an
  `OperatorAuthorityLease` response or a typed rejection reason.
- [x] T063 [US4] Add a headless smoke path where the GS requests and applies
  a control lease through the NDNSF service path.
- [x] T064 [US4] Add MiniNDN harness support and documentation for
  `--auto-authority-issuer-test`.
- [x] T065 Run build, focused C++ unit test, Python envelope regression,
  whitespace check, and authority-issuer MiniNDN smoke.

## Phase 17: Multi-Operator Lease Arbitration

- [x] T066 [US4] Add an issuer-side active lease table for leases issued
  through `/UAV/GS/OperatorAuthority/Lease`.
- [x] T067 [US4] Reject conflicting exclusive control/mission/admin lease
  requests for the same drone unless the requester is renewing its own lease.
- [x] T068 [US4] Allow monitor leases without exclusivity and allow admin
  requests to override existing exclusive leases.
- [x] T069 [US4] Return typed arbitration metadata for rejection/override:
  conflicting operator, conflicting lease id, active count, and overridden count.
- [x] T070 [US4] Add MiniNDN harness support and documentation for
  `--auto-authority-arbitration-test`.
- [x] T071 Run build, focused C++ unit test, Python envelope regression,
  whitespace check, and authority-arbitration MiniNDN smoke.

## Phase 18: Authority Persistence and Admin Policy

- [x] T072 [US4] Add issuer-side active lease persistence through a configured
  `operator-authority-state-file`.
- [x] T073 [US4] Add an `operator-admin-ids` allowlist for admin-scope lease
  override requests while preserving the existing permissive default when no
  allowlist is configured.
- [x] T074 [US4] Return admin override evidence with revoked lease IDs and
  revoked operators in the typed lease response.
- [x] T075 [US4] Add a headless authority persistence smoke that verifies
  unauthorized admin rejection, authorized admin override, revoked evidence,
  and persisted active lease state.
- [x] T076 [US4] Add MiniNDN harness support and documentation for
  `--auto-authority-persistence-test`.
- [x] T077 Run build, focused C++ unit test, Python envelope regression,
  whitespace check, and authority-persistence MiniNDN smoke.

## Phase 19: Fetchable Revocation Records

- [x] T078 [US4] Add an app-level
  `/UAV/GS/OperatorAuthority/Revocation` service name, runtime config field,
  and controller policy entries.
- [x] T079 [US4] Store admin override revocation records with revoked lease,
  revoked operator, revoker operator, target drone, scope, and timestamp.
- [x] T080 [US4] Persist revocation records beside active issuer leases in the
  configured authority state file.
- [x] T081 [US4] Register a GS revocation lookup service that returns typed
  `found=true/false` response fields for a requested revoked lease ID.
- [x] T082 [US4] Add a headless authority revocation smoke and MiniNDN harness
  support for `--auto-authority-revocation-test`.
- [x] T083 Run build, focused C++ unit test, Python envelope regression,
  whitespace check, and authority-revocation MiniNDN smoke.

## Phase 20: Client-Side Revocation Refresh

- [x] T084 [US4] Add a client-side authority lease refresh helper that queries
  `/UAV/GS/OperatorAuthority/Revocation` for the active lease ID.
- [x] T085 [US4] Mark the local active operator lease as revoked when refresh
  returns a matching revocation record, so existing command/mission gates
  return `lease-revoked`.
- [x] T086 [US4] Add a headless authority refresh smoke where one operator gets
  control, an authorized admin overrides it, and the original operator detects
  revocation through refresh.
- [x] T087 [US4] Add MiniNDN harness support and quickstart docs for
  `--auto-authority-refresh-test`.
- [x] T088 Run build, focused C++ unit test, Python envelope regression,
  whitespace check, and authority-refresh MiniNDN smoke.

## Phase 21: GUI and Periodic Authority Refresh

- [x] T089 [US4] Add `operator-authority-refresh-interval-ms` config/CLI support
  with `0` as the disabled default.
- [x] T090 [US4] Add a runtime periodic refresh loop that reuses
  `refreshOperatorAuthorityLeaseFromIssuer()` and avoids overlapping refreshes.
- [x] T091 [US4] Add a `Refresh Lease` GUI button and show manual/periodic
  refresh state in the Operator Authority inspector.
- [x] T092 [US4] Add MiniNDN harness support and docs for
  `--auto-authority-refresh-timer-test`.
- [x] T093 Run build, focused C++ unit test, Python envelope regression,
  whitespace check, and authority-refresh-timer MiniNDN smoke.

## Phase 22: Operator Authority Alert History

- [x] T094 [US4] Add a bounded operator-authority alert history to ground
  station runtime state.
- [x] T095 [US4] Record alert entries when admin override creates a revocation
  record and when client refresh detects a revoked active lease.
- [x] T096 [US4] Show recent authority alerts in the Operator Authority
  inspector.
- [x] T097 [US4] Add MiniNDN harness support and docs for
  `--auto-authority-alert-history-test`.
- [x] T098 Run build, focused C++ unit test, Python envelope regression,
  whitespace check, and authority-alert-history MiniNDN smoke.

## Phase 23: Operator Authority Audit Trail Persistence

- [x] T099 [US4] Persist bounded operator-authority alert entries in the
  configured authority state file beside active leases and revocation records.
- [x] T100 [US4] Reload persisted alert entries at startup so post-mission
  review can inspect admin override and revocation-detected evidence after a
  process restart.
- [x] T101 [US4] Extend the authority alert-history headless smoke to clear
  in-memory alerts, reload the authority state file, and verify the audit
  entries survive.
- [x] T102 [US4] Update quickstart and README docs to describe the persisted
  audit trail evidence.
- [x] T103 Run build, focused C++ unit test, Python envelope regression,
  whitespace check, and authority-alert-history MiniNDN smoke.

## Phase 24: Operator Authority Audit Query Service

- [x] T104 [US4] Add a configurable UAV app-level
  `/UAV/GS/OperatorAuthority/Audit` service name and policies.
- [x] T105 [US4] Register an NDNSF provider service that returns the recent
  bounded authority alert audit entries as key-value response fields.
- [x] T106 [US4] Add a ground-station sync helper and headless smoke that
  queries the audit trail through the NDNSF service path after admin override
  and revoked-lease refresh.
- [x] T107 [US4] Add MiniNDN harness support and docs for
  `--auto-authority-audit-query-test`.
- [x] T108 Run build, focused C++ unit test, Python envelope regression,
  whitespace check, and authority-audit-query MiniNDN smoke.

## Phase 25: Operator Authority Audit Query Windowing

- [x] T109 [US4] Extend the audit query request fields with `offset`,
  `limit`, `from_ms`, and `to_ms` while keeping the existing recent-entry
  default behavior.
- [x] T110 [US4] Return `matched_count`, `returned_count`, `offset`, `limit`,
  and the applied time bounds in the audit response.
- [x] T111 [US4] Extend the headless audit smoke to query a one-entry page
  after the full query and verify it returns the expected
  `lease-revoked-detected` alert.
- [x] T112 [US4] Update docs for bounded audit query pagination and time-window
  filtering.
- [x] T113 Run build, focused C++ unit test, Python envelope regression,
  whitespace check, and authority-audit-query MiniNDN smoke.

## Phase 26: Operator Authority Audit Redaction

- [x] T114 [US4] Add explicit `redaction=full|summary|self` and
  `requester_operator` query fields while keeping `full` as the default for
  backward compatibility.
- [x] T115 [US4] Redact `revoked_operator` and `revoker_operator` in
  `summary` and non-matching `self` responses while preserving event type,
  reason, lease, drone, scope, and timestamp context.
- [x] T116 [US4] Extend the headless audit smoke to verify that summary
  redaction hides operator identities and preserves event context.
- [x] T117 [US4] Update docs for audit redaction behavior and the remaining
  authenticated requester-policy gap.
- [x] T118 Run build, focused C++ unit test, Python envelope regression,
  whitespace check, and authority-audit-query MiniNDN smoke.

## Phase 27: Operator Authority Audit Requester Binding

- [x] T119 [US4] Pass the NDNSF requester identity from the audit service
  handler into the local audit query implementation.
- [x] T120 [US4] Prefer an operator id derived from authenticated requester
  identity over caller-supplied `requester_operator`, while preserving payload
  fallback for unmapped identities.
- [x] T121 [US4] Return `requester_identity`, `effective_requester_operator`,
  and `requester_operator_source` in audit responses for diagnostics.
- [x] T122 [US4] Extend the headless audit smoke to prove that a spoofed
  payload requester does not override the authenticated requester mapping.
- [x] T123 Run build, focused C++ unit test, Python envelope regression,
  whitespace check, and authority-audit-query MiniNDN smoke.

## Phase 28: Operator Authority Full Audit Gate

- [x] T124 [US4] Require admin authority for `redaction=full` when the
  requester identity maps to an operator.
- [x] T125 [US4] Preserve backward compatibility for unmapped requester
  identities by leaving their default `full` behavior unchanged.
- [x] T126 [US4] Return a clear `full-redaction-requires-admin` audit response
  instead of exposing full operator identities to non-admin operators.
- [x] T127 [US4] Extend the headless audit smoke to verify that non-admin
  requester identities can use `summary`/`self` but cannot use `full`.
- [x] T128 Run build, focused C++ unit test, Python envelope regression,
  whitespace check, and authority-audit-query MiniNDN smoke.

## Dependencies

- T004-T005 block all user story implementation.
- US1-US4 are independently testable after T004-T005.
- Validation tasks must run after all user story tests are added.

## Next Implementation Stage

Future tasks should add a second MiniNDN ground-station identity to exercise
cross-GS operator-audit policy instead of only same-GS requester mapping.
