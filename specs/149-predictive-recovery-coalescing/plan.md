# Implementation Plan: Predictive Recovery Coalescing

## Technical Context

- C++17 Core in `StreamFacade.*` and `Stream.hpp`
- pybind11 status binding
- existing UAV Drone/Ground Station and Spec 148 MiniNDN runner
- Boost 1.71 build environment

## Design

1. Preserve the existing predictive wire objects and one-XOR repair.
2. Replace per-cursor frontier fetches with a session-level waiter set and one
   pending Interest.
3. Cache validated group commits by exact Data name. Before expressing an
   Interest, serve from cache; while a name is pending, attach another lookup
   waiter instead of expressing again.
4. Prune cached/pending names against each newly verified frontier retention
   list and clear all state on stop.
5. Separate recovery-control accounting from Mapping and expose:
   `recoveryControlInterests`, `recoveryFrontierInterests`,
   `recoveryGroupInterests`, `recoveryCoalescedWaiters`, and
   `recoveryMetadataCacheHits`.
6. Reuse the Spec 148 runner/analyzer architecture in new files/output only.
   Freeze the same two profiles before execution; no automatic retry.

## Ownership

| Concern | Owner |
|---|---|
| App-signed Payload and flush boundary | Application |
| adaptive future scheduling | existing Core controller, unchanged |
| recovery metadata coalescing/cache | generic Core |
| workload, decode, GUI | UAV-APP |
| formal orchestration/analysis | experiment runner/analyzer |

## Validation

- Native deterministic tests use a controlled Face to create simultaneous
  gaps and assert actual expressed-Interest counts.
- Python verifies the symmetric status surface.
- Full native build/test follows focused tests.
- Fresh MiniNDN uses `AI_Lab.conf`, controller+GS on `memphis`, Drone on
  `ucla`, real `drone.mp4`, 5-second warmup and >=60-second measurement.
- Spec 148 campaign is read-only baseline evidence.

## Rollback

Rollback restores the pre-Spec-149 binary. No persisted format or public API
changes are introduced.

## Constitution Check

PASS: generic Core ownership, exact security path, CodeGraph-first analysis,
cohesive tasks, MiniNDN final verification, and immutable negative evidence.
