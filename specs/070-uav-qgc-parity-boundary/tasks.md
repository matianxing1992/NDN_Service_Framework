# Tasks: UAV QGC-Parity Boundary Slice

## Phase 1: Design and Boundary

- [x] T001 Review existing UAV operational state contracts and QGC-parity gaps.
- [x] T002 Use CodeGraph before broad code edits.
- [x] T003 Perform a second-pass checklist and retain maintainer authority over
  the final architecture.
- [x] T004 Update `docs/ndnsf-core-app-boundary.md` with the QGC-parity split.

## Phase 2: Protocol Contracts

- [x] T005 Add `VehicleParameterEditRequest` and validation helper.
- [x] T006 Add `VehicleParameterEditResult` and success helper.
- [x] T007 Add `PreflightCheckItem` and blocking-failure helper.
- [x] T008 Add `MavlinkMessageSummary` and stale/active helper.
- [x] T009 Add `UavAnalyzeSnapshot` with flattened message summaries.

## Phase 3: Tests

- [x] T010 Add parameter-edit request/result round-trip tests.
- [x] T011 Add preflight checklist round-trip and blocking tests.
- [x] T012 Add MAVLink analyze snapshot round-trip and active-message tests.

## Phase 4: Validation

- [x] T013 Build unit tests.
- [x] T014 Run focused `UavProtocolState` tests.
- [x] T015 Run Python app-core envelope migration regression.
- [x] T016 Run `git diff --check`.
- [x] T017 Commit the completed slice.

## Phase 5: Parameter Edit Runtime Slice

- [x] T018 Add `/UAV/MAVLink/ParameterEdit` service suffix and config plumbing.
- [x] T019 Add mock flight-controller parameter write/verify support.
- [x] T020 Register drone provider parameter-edit service.
- [x] T021 Add ground-station parameter-edit async/sync helpers.
- [x] T022 Add headless `--auto-parameter-edit-test` flow.
- [x] T023 Add MiniNDN harness flag and success marker.
- [x] T024 Build UAV apps and unit tests.
- [x] T025 Run focused C++ protocol tests.
- [x] T026 Run Python envelope regression.
- [x] T027 Run parameter-edit MiniNDN smoke when practical.
- [x] T028 Run `git diff --check`.
- [x] T029 Commit the runtime slice.

## Phase 6: Preflight Checklist Runtime Slice

- [x] T030 Add `/UAV/Preflight/Checklist` service suffix and config plumbing.
- [x] T031 Add drone-side checklist generation from telemetry/readiness/camera state.
- [x] T032 Register drone provider preflight checklist service.
- [x] T033 Add ground-station preflight request/cache/sync helpers.
- [x] T034 Add headless `--auto-preflight-checklist-test` flow.
- [x] T035 Add MiniNDN harness flag and success marker.
- [x] T036 Build UAV apps and unit tests.
- [x] T037 Run focused C++ protocol tests.
- [x] T038 Run Python envelope regression.
- [x] T039 Run preflight checklist MiniNDN smoke.
- [x] T040 Run `git diff --check`.
- [x] T041 Commit the preflight runtime slice.

## Phase 7: MAVLink Analyze Snapshot Runtime Slice

- [x] T042 Add `/UAV/MAVLink/AnalyzeSnapshot` service suffix and config plumbing.
- [x] T043 Add drone-side analyze snapshot generation from telemetry, mission, and video state.
- [x] T044 Register drone provider analyze snapshot service.
- [x] T045 Add ground-station analyze snapshot request/cache/sync helpers.
- [x] T046 Add headless `--auto-analyze-snapshot-test` flow.
- [x] T047 Add MiniNDN harness flag and success marker.
- [x] T048 Build UAV apps and unit tests.
- [x] T049 Run focused C++ protocol tests.
- [x] T050 Run Python envelope regression.
- [x] T051 Run analyze snapshot MiniNDN smoke.
- [x] T052 Run `git diff --check`.
- [x] T053 Commit the analyze snapshot runtime slice.

## Phase 8: Operator Dashboard Snapshot Runtime Slice

- [x] T054 Add `UavOperatorDashboardSnapshot` protocol contract.
- [x] T055 Add ground-station dashboard aggregation from telemetry, parameters, preflight, analyze, and action gates.
- [x] T056 Add headless `--auto-dashboard-snapshot-test` flow.
- [x] T057 Add MiniNDN harness flag and success marker.
- [x] T058 Build UAV apps and unit tests.
- [x] T059 Run focused C++ protocol tests.
- [x] T060 Run Python envelope regression.
- [x] T061 Run dashboard snapshot MiniNDN smoke.
- [x] T062 Run `git diff --check`.
- [x] T063 Commit the dashboard snapshot runtime slice.

## Phase 9: Ground Station Dashboard Panel Slice

- [x] T064 Add a Vehicle Summary inspector panel that consumes `UavOperatorDashboardSnapshot`.
- [x] T065 Add `OPERATOR_DASHBOARD_PANEL_STATE` logging for GUI verification.
- [x] T066 Add GUI `--auto-dashboard-panel-test` flow.
- [x] T067 Add MiniNDN harness flag and success marker.
- [x] T068 Build UAV apps and unit tests.
- [x] T069 Run focused C++ protocol tests.
- [x] T070 Run Python envelope regression.
- [x] T071 Run dashboard panel MiniNDN GUI smoke.
- [x] T072 Run `git diff --check`.
- [x] T073 Commit the dashboard panel slice.

## Phase 10: Ground Station Detail Panels Slice

- [x] T074 Add Preflight Checks inspector panel from cached preflight checklist rows.
- [x] T075 Add MAVLink Messages inspector panel from cached Analyze snapshot rows.
- [x] T076 Add `OPERATOR_DETAIL_PANEL_STATE` logging for GUI verification.
- [x] T077 Add GUI `--auto-dashboard-detail-panel-test` flow.
- [x] T078 Add MiniNDN harness flag and success marker.
- [x] T079 Build UAV apps and unit tests.
- [x] T080 Run focused C++ protocol tests.
- [x] T081 Run Python envelope regression.
- [x] T082 Run dashboard detail panel MiniNDN GUI smoke.
- [x] T083 Run `git diff --check`.
- [x] T084 Commit the dashboard detail panel slice.

## Phase 11: Ground Station Detail Refresh Buttons Slice

- [x] T085 Add explicit Preflight and Analyze refresh buttons to the Ground Station toolbar.
- [x] T086 Route button clicks through the same NDNSF request helpers as the panels.
- [x] T087 Add button-result logs for preflight and Analyze refreshes.
- [x] T088 Add GUI `--auto-dashboard-refresh-buttons-test` flow.
- [x] T089 Add MiniNDN harness flag and success marker.
- [x] T090 Build UAV apps and unit tests.
- [x] T091 Run focused C++ protocol tests.
- [x] T092 Run Python envelope regression.
- [x] T093 Run dashboard refresh buttons MiniNDN GUI smoke.
- [x] T094 Run `git diff --check`.
- [x] T095 Commit the dashboard refresh buttons slice.

## Phase 12: Ground Station Parameter Edit Panel Slice

- [x] T096 Add editable parameter controls for name, expected value, requested value, and MAVLink type.
- [x] T097 Route the Apply Param button through the same NDNSF parameter-edit service as the headless test.
- [x] T098 Add GUI result/cache logs for parameter edit verification.
- [x] T099 Add GUI `--auto-parameter-edit-panel-test` flow.
- [x] T100 Add MiniNDN harness flag and success marker.
- [x] T101 Build UAV apps and unit tests.
- [x] T102 Run focused C++ protocol tests.
- [x] T103 Run Python envelope regression.
- [x] T104 Run parameter edit panel MiniNDN GUI smoke.
- [x] T105 Run `git diff --check`.
- [x] T106 Commit the parameter edit panel slice.
