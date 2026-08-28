# Tasks: Three-Provider Mobility Baselines

## Phase 1: Evidence and contract freeze

- [x] T001 Freeze the six-cell experiment, no-NDNSF boundary, original source hashes, single-writer state and evidence limitations in `specs/169-three-provider-mobility-baselines/` before source changes.

## Phase 2: Parallel baseline clients

- [x] T002 [P] [US1] Add the three-endpoint health-aware gRPC client, backward-compatible health/counter support and focused failover/accounting tests in `Experiments/gRPC/` and `tests/python/`, then pass the focused gate.
- [x] T003 [P] [US2] Add request-scoped three-Provider NSC timeout failover, stale-attempt fencing, structured accounting and focused build/tests in `Experiments/NDN_NSC/` and `tests/`, then pass the focused gate.

## Phase 3: Matched harness and smoke

- [x] T004 [US3] Wire both three-Provider clients into one deterministic no-oracle MiniNDN harness with dry-run, smoke, exact manifest and aggregate contracts in `Experiments/WifiRouterMobilityReliability.py`; pass syntax/tests and one sub-10-second smoke per system.

## Phase 4: One-shot formal campaign

- [x] T005 [US3] Execute exactly the six registered 60-second NSC/gRPC cells once into a unique `results/wifi_router_mobility_three_provider_*` path, retain negative cells without retry, validate all campaign artifacts and record the post-implementation evidence under `specs/169-three-provider-mobility-baselines/evidence/`.

## Dependencies

- T001 precedes all implementation.
- T002 and T003 are parallel and independently testable.
- T004 depends on both client CLIs.
- T005 depends on all focused tests and both smoke gates.

## Fragmentation Review

Each task closes one independently reviewable behavior and includes its own
tests/evidence. Test writing, implementation and validation are intentionally
coalesced rather than represented as mechanical subtasks.
