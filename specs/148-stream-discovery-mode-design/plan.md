# Implementation Plan: Predictive Stream API Replacement

**Branch**: `148-stream-discovery-mode-design`  
**Date**: 2026-07-25  
**Readiness**: READY — corrected pre-implementation audit passed

## Summary

Replace the old public high-level stream workflow with one predictive workflow:
`start()` + `push(exact App-signed Data)` + `flush()`. Remove the old C++ and
Python methods and migrate every repository caller atomically. Preserve generic
low-level Core primitives when useful, but do not expose them as a competing
application API.

## Code Facts Driving the Plan

- `StreamPublisher` currently contains both old and predictive methods.
- Python currently exposes old `start/announce/publish` plus
  `start_predictive/push/flush`.
- Current `push()` passes the supplied signed wire into a lower-level
  publication path that can wrap/re-sign it; exact-wire ownership is not yet
  proven.
- Current `flush()` does not yet provide a complete authenticated,
  unequal-length-safe group contract.
- Current predictive subscriber admission does not yet invoke the configured
  validator.
- UAV Drone has a partial dual path; Ground Station and the MiniNDN mode flag
  are not fully wired.
- Focused predictive tests currently fail and no fresh Spec 148 MiniNDN result
  exists.

## Architecture Decisions

1. The public facade is replaced, not extended.
2. `PredictiveStreamDescriptor` is the sole descriptor returned by high-level
   `start()`.
3. App-signed source Data remains byte-identical through Core.
4. Core owns routing, retention, repair metadata/packets, frontier, scheduling,
   retry accounting, and lifecycle.
5. Existing generic adaptive prefetch and FEC primitives are reused; no
   workload-specific branch is permitted.
6. Old/new comparison uses immutable separate binaries. Product code does not
   keep a Mapping-first API for benchmarking.
7. Rollback means deploying the previous binary, not switching a compatibility
   flag.

## Implementation Phases

### Phase 0 — Replacement gate

Freeze the corrected API contract, inventory every old facade call site, add
negative compile/source checks, and make the intended breaking migration
explicit.

### Phase 1 — Core exact-wire publication

Implement the sole `start/push/flush` surface, exact-wire routing/retention,
authenticated repair/frontier metadata, concurrency, stop fencing, and
fail-closed validation.

### Phase 2 — Consumer and language parity

Complete predictive validation, adaptive future-Interest scheduling,
repair/retry/skip behavior, observability, pybind11/Python parity, and migrate
unit tests/examples.

### Phase 3 — Application migration

Remove UAV dual-path behavior. Drone publishes only through `push/flush`;
Ground Station subscribes only through the predictive descriptor. Remove the
unused discovery-mode illusion. Add structured provider/consumer runtime proof
defined by `contracts/uav-minindn-acceptance.md`.

### Phase 4 — Verification

Rebuild Core and Python binding, run complete tests, perform source/API removal
audits, execute a real three-process/two-node UAV smoke, then execute fresh
zero-loss and fixed light-loss/reordering 60-second MiniNDN cells. Write closure
only if every required artifact and metric exists.

## Validation Strategy

- Compile-negative tests for removed C++ signatures.
- Python attribute/API tests proving old names are absent.
- Unit tests for exact signed wire, validation, group metadata, unequal source
  lengths, concurrency, stop, late join, and recovery ordering.
- Full native/Python regression after all caller migrations.
- Real two-node MiniNDN UAV smoke and a fresh metrics report.
- The smoke launches `App_ServiceController` + `UavGroundStationApp` on
  `memphis` and `UavDroneApp` on `ucla`; quick-smoke is preflight only.
- Formal validation uses the two cells and artifact schema frozen in
  `contracts/uav-minindn-acceptance.md`.
- Optional pinned-old versus new matched comparison with both binary hashes.

## Migration and Rollback

Migration is atomic across Core, bindings, examples, tests, and UAV-APP.
Intermediate compilation breakage is resolved within the same feature; no
compatibility alias is introduced. Rollback deploys the pinned pre-Spec-148
binary/commit. Frozen Spec 125/144/146/147 evidence remains untouched.

## Constitution Check — PASS

The corrected documents define the breaking migration, ownership, rollback,
security, complete caller migration, real UAV runtime, and admissible MiniNDN
evidence. Remaining prototype defects are implementation tasks and do not block
starting the work; they continue to block closure.
