# Tasks: Paper-Aligned Prefetch Control

## Phase 1: Stable controller

- [x] T001 [US1] Add failing traces for detection hold, current-time hold telemetry, and restore-after-over-adjustment; implement the controller correction and pass the focused gate in `tests/unit-tests/stream.t.cpp` and `ndn-service-framework/Stream.{hpp,cpp}`.

## Phase 2: Network-only delay

- [x] T002 [US2] Add failing direct-versus-ahead delay cases; implement phase-aware payload-delay observation, connect it before validation/APP processing, and clarify the consumer counter semantics while preserving APIs in `tests/unit-tests/stream.t.cpp` and `ndn-service-framework/Stream.{hpp,cpp}`.

## Phase 3: Network evidence and closeout

- [x] T003 [US3] Run focused Stream and UAV regressions, execute one unique 60-second zero-loss MiniNDN UAV result without replacing failures, audit it against every gate, and record findings and remaining paper differences in `results/spec124-*` and `specs/124-paper-aligned-prefetch-control/completion-summary.md`.

## Dependencies

- T001 and T002 are sequential because they share controller state and tests.
- T003 starts only after both focused behaviors pass.

## Cohesion audit

The three tasks are independently reviewable behaviors. Test, implementation, and focused acceptance for each behavior stay together; no file-edit or command-only fragments are created.
