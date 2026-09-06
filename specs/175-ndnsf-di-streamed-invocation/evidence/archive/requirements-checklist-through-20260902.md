# Archived Requirements Checklist Through 2026-09-02

> Historical checklist only. Use `../../checklists/requirements.md`.

**Purpose**: Validate specification completeness and quality before planning

**Created**: 2026-08-21

**Feature**: [spec.md](../spec.md)

## Content Quality

- [x] No incidental implementation detail; named protocol/runtime boundaries are
  externally verifiable constraints explicitly required by the feature.
- [x] Focused on application and operator outcomes.
- [x] Written so each protocol term is introduced through observable behavior.
- [x] All mandatory sections completed.

## Requirement Completeness

- [x] No `[NEEDS CLARIFICATION]` markers remain.
- [x] Requirements are testable and unambiguous.
- [x] Success criteria are measurable.
- [x] Success criteria describe observable outcomes and qualification verdicts.
- [x] All acceptance scenarios are defined.
- [x] Edge cases are identified.
- [x] Scope is clearly bounded.
- [x] Dependencies and assumptions are identified.

## Feature Readiness

- [x] All functional requirements have clear acceptance criteria.
- [x] User scenarios cover unary compatibility, streamed generation, recovery,
  multi-turn continuation/tiered state, and staged qualification.
- [x] Feature meets measurable outcomes defined in Success Criteria.
- [x] Exact API, wire, defaults, topology, workload, and command details are
  delegated to the plan and versioned contracts rather than duplicated here.

## Validation-order gate

- [x] Design-to-code convergence is a hard prerequisite for formal validation:
  freeze the design and effective configuration, inspect the real production
  path and dependencies, record and repair every discrepancy, run only the
  focused repair regressions, re-audit to a fresh `PASS`, and only then accept
  complete suites, MiniNDN, benchmarks, SIF, or Tiger results.
- [x] A broad test run started while convergence is `BLOCK` is diagnostic
  history only; it cannot close a task, qualify a candidate, or support a paper
  claim. Any later behavior-affecting change invalidates the `PASS` and repeats
  this gate.

## Notes

- Validation iteration 1 passed on 2026-08-21.
- The explicit ONNX-only runtime and one-Provider/one-role language is retained
  because it is a user-approved architecture boundary and a directly testable
  deployment property, not an incidental coding choice.
- The 20 token/s threshold is a named performance-qualification verdict. A
  functionally correct result below that threshold remains reportable but cannot
  be labeled `PERFORMANCE_PASS`.
- Validation iteration 2 passed on 2026-08-25 after the Provider-owned
  decode-state correction. The specification now distinguishes an implemented
  state container from production runtime ownership and requires automatic
  prefill commit, seven decode hits for the eight-token oracle, atomic successor
  commit, bounded pin/eviction/release behavior, and explicit failure on a
  missing or mismatched predecessor state.
- Correct token output alone is not accepted as effective KV-cache evidence.
  CUDA qualification also requires persistent device-resident state, zero
  complete-state host round trips during decode, no decode-state bytes on NDN
  dependency edges, and a same-artifact paired cached-versus-full-prefix control.
- This checklist validates specification quality, not implementation closure.
  T005--T019, T024, and T029--T033 must complete their implementation and
  focused tests before T020 starts the one formal G0--G2 sequence.
- Validation iteration 3 passed on 2026-08-25 after closing the identity-source
  and lineage ambiguity. The specification now requires sealed static identity
  authority, a common canonical consumed-token prefix across all roles, a
  runtime-derived position commitment, explicit state/predecessor inference
  epochs, and zero runner calls after a predecessor mismatch. Activation bytes,
  free-form runner metadata, session IDs, and loop counters alone cannot
  authorize cache reuse.
- Validation iteration 4 passed on 2026-08-25 after separating cache integrity
  from cache effectiveness. The CPU gate now requires a matched semantic
  control with exact output parity, actual cached input extents, and positive
  prefix work avoided without using noisy CPU timing; G5 retains the real CUDA
  device-residency and alternating-order compute-time requirement. T024 also
  binds cumulative G4/G4T/G5U/G5/G6 prerequisites to the exact submitted candidate
  before any network access or allocation.
- Validation iteration 5 passed on 2026-08-26 after adding explicit multi-turn
  continuation. The specification keeps request-scoped V1 identity intact and
  separately freezes full versus appended input, opaque aggregate checkpoints,
  Provider role receipts, same-placement reuse, all-role transactional commit,
  linear context epochs, explicit full-prefill fallback, and bounded GPU/host
  residency. It also fixes I16--I20, M11--M14, and G6C so all source and cheap
  validation finish before the single final SIF is built. This is a design-
  quality pass, not evidence that T029--T034 are implemented.
- Validation iteration 6 passed on 2026-08-26 after correcting execution order.
  The specification now completes every Core/runtime/API/conversation and
  launcher/harness implementation task before formal G0--G3 qualification;
  historical MiniNDN/SIF failures remain regression inputs rather than active
  campaign work. T024/T032/T033 close source and mutation-test boundaries,
  while T020/T022/T023/T025 execute the frozen qualification subject.
- Validation iteration 7 passed on 2026-08-26 after separating request-local
  decode state from conversation-scoped cross-request state. The specification
  now requires exact completed-turn prefix finalization, atomic all-role state
  ownership promotion, fresh authority for the next turn, separate metrics,
  direct request-cache reuse negatives, and parent preservation on failed
  promotion. This is a contract-quality result; T029--T033 remain implementation
  work.
- Validation iteration 8 passed on 2026-08-26 after making the state-scope
  boundary explicit at the feature and validation-contract level. The two
  stores now have distinct keys, creation rules, reuse rules, evidence fields,
  and failure semantics; a request-local hit cannot satisfy a conversation
  continuation claim.
- Validation iteration 9 passed on 2026-08-30 after converting recurring
  Tiger deployment lessons into testable candidate-closure requirements. The
  SIF is no longer treated as the complete release identity: source, exact SIF,
  host replay, submit bundle, effective configuration, model artifacts, and
  validation contract form one immutable tuple. The spec now requires
  deterministic plane invalidation, a no-side-effect pre-dispatch mutation
  suite, same-identity live repository ACK readiness, and terminal child/cleanup
  evidence. At that historical checkpoint T035--T036 remained open
  implementation tasks; the current implementation boundary is closed and
  their focused evidence is recorded in the audit.
- Validation iteration 10 recorded the Provider launch-order correction on
  2026-08-30. Readiness now requires native handler/filter registration and
  event-loop start before the READY marker, and the repository probe records
  bounded fresh-request retries. The M14 diagnostic passed after this fix, but
  it invalidated the earlier source/SIF identity; a new G0--G4 chain is still
  required.
- Validation iteration 11 passed for document quality on 2026-08-31 after a
  code-aware production-path audit. The then-current implementation verdict
  was BLOCK, not PASS: T020/T022/T031 were reopened and T037--T042 required an
  ordinary-V3 fail-closed boundary, post-Selection native role assembly, real
  native sampling/detokenization, distributed device-state retention, physical
  conversation D2H/H2D movement, filterable `NDN_LOG` diagnostics, and tests
  that exercise those production owners. This paragraph is historical; the
  repairs and fresh audit are recorded below.
- Validation iteration 12 passed on 2026-09-01 after the rank-one ordinary-V3
  candidate correction and adapter-owned state-transfer audit. T031, T035--T036,
  and T037--T042 are closed at their implementation/convergence boundaries;
  T020 is closed for the fresh `20260901-v3-boundary-fix` G0/G1/G2 subject.
  T022's 42-process host/CPU matrix is still running, so SIF/Tiger evidence
  remains deferred and no release or performance claim is implied.
