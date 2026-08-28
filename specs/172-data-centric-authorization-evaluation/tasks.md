# Tasks: Data-Centric Authorization Evaluation

**Input**: [spec.md](spec.md), [plan.md](plan.md), [research.md](research.md),
[data-model.md](data-model.md), and [contracts/](contracts/)

## Phase 1: Foundational evidence contract

- [x] T001 Build and validate the registered case/manifest/claim contract by adding schema and registry checks, exact runtime test selectors, and a baseline source-hash inventory in `tests/python/test_authorization_evaluation.py`, `specs/172-data-centric-authorization-evaluation/contracts/`, and `specs/172-data-centric-authorization-evaluation/quickstart.md`; accept when all registered cases are parseable, every expected gate maps to an existing runtime boundary, and the existing focused security tests can be addressed by exact selectors.

---

## Phase 2: User Story 1 - Defensible contribution framing

**Goal**: Make the paper state the data-centric and authorization contributions without claiming new cryptography, eliminating trust chains, or overstating onboarding.

**Independent Test**: Build the paper and map every revised contribution sentence to `contracts/claim-evidence-matrix.md` and a primary source or current implementation invariant.

- [x] T002 [US1] Revise the abstract, introduction, background, design, related work, high-level comparison, and conclusion as one coherent claim package in `docs/PAPER/named-data-network-service-framework-paper/sections/{abstract,introduction,background,design,relatedWork,evaluation,conclusion}.tex` and `sections/references.bib`; include the three-layer security model, the producer-scoped four-Data transaction, a primary-source comparison table, explicit current limitations, and visibly labeled `TBD` evaluation tables, then build `NDNSF.tex` and update `contracts/claim-evidence-matrix.md` with exact paper locations.

---

## Phase 3: User Story 2 - Reproducible authorization evidence

**Goal**: Demonstrate the composed allow/deny semantics and prove that denied cases never execute the service handler.

**Independent Test**: One deterministic smoke run executes every registered case with 100% expected/observed agreement and zero handler executions for denied cases.

- [x] T003 [US2] Implement the correctness experiment as a cohesive test-first slice in `Experiments/run_authorization_evaluation.py`, `Experiments/analyze_authorization_evaluation.py`, `tests/python/test_authorization_evaluation.py`, and the relevant focused C++ tests under `tests/unit-tests/`; reuse the production Controller/User/Provider security path, emit per-case gate and handler-count evidence plus a validated manifest, run the affected unit tests and `examples/run_security_regressions.sh`, and retain only the canonical correctness smoke result.

---

## Phase 4: User Story 3 - Provider-decoupled onboarding evidence

**Goal**: Establish exactly which Provider state remains unchanged when a new User is authorized and which automatic refresh is still required.

**Independent Test**: Three repetitions preserve Provider binary/service/identity/trust hashes, expose stale-epoch rejection, install current controller material without per-User Provider configuration, and complete the new User's first request.

- [x] T004 [US3] Implement and execute the offline-Provider/new-User onboarding transition in `Experiments/run_authorization_evaluation.py`, `Experiments/analyze_authorization_evaluation.py`, `tests/python/test_authorization_evaluation.py`, and only if the registered transition proves it necessary, the minimal policy-refresh path in `ndn-service-framework/{ServiceController,ServiceProvider,ServiceUser}.{cpp,hpp}`; record manual changes, automatic refreshes, control bytes, epochs, local hashes, and time to first success, run all affected security gates, and update the ONB claims and paper limitation/result cells without exceeding the evidence.

---

## Phase 5: User Story 4 - Authorization cost and scaling evidence

**Goal**: Quantify cold/warm authorization cost without confounding service execution or transport behavior.

**Independent Test**: Matched scale points report run-level latency distributions, crypto counters, wire/state cost, failures, and reproducibility variation with identical workload and network conditions.

- [x] T005 [US4] Add and run the matched cold/warm overhead subject and analyzer in `Experiments/run_authorization_evaluation.py`, `Experiments/analyze_authorization_evaluation.py`, and `tests/python/test_authorization_evaluation.py`, using existing runtime crypto counters and a test-only comparison boundary that cannot disable production authorization; retain canonical run manifests for registered User/Provider/policy scale points and replace only supported COST cells and prose in `docs/PAPER/named-data-network-service-framework-paper/sections/evaluation.tex`.

---

## Phase 6: Network confirmation and claim convergence

- [x] T006 Confirm the composed authorized, denied, and onboarding behaviors over MiniNDN using the shared experiment conventions in `Experiments/run_authorization_evaluation.py` and a canonical `results/spec172_*` campaign; preserve exact commands, source/config hashes, terminal status, traces, and summarized evidence, and do not promote local-only evidence to a network claim.
- [x] T007 Complete the paper/evidence audit by validating every manifest and artifact hash, reproducing deterministic cases, applying the statistical fallacy scan to timed results, updating every claim status in `specs/172-data-centric-authorization-evaluation/contracts/claim-evidence-matrix.md`, replacing or retaining every `TBD` in the paper accordingly, rebuilding `docs/PAPER/named-data-network-service-framework-paper/NDNSF.tex`, and recording unresolved revocation, Controller trust, policy-refresh, and scalability limits.
- [x] T008 Produce ten substantive body pages, excluding the References page, without global font reduction; remove only redundant background, superseded diagnostics, and repeated result prose, while preserving or restoring the producer-scoped four-Data transaction, three-layer authorization model, threat boundaries, FirstResponding sequence, runtime/API design, reproducibility controls, registered authorization evidence, bounded mobility/discovery result, Provider-work result, limitations, and supporting citations; rebuild and visually audit all eleven final PDF pages.

## Dependencies

```text
T001 -> T002
T001 -> T003 -> T004 -> T005
T003 + T004 -> T006
T002 + T005 + T006 -> T007
T007 -> T008
```

- T002 may proceed in parallel with T003 after T001 because its initial tables
  remain `TBD`.
- T004 depends on the composed correctness evidence from T003.
- T005 depends on stable instrumentation and manifest semantics from T003.
- T006 starts only after local correctness and onboarding semantics are stable.
- T007 is the only task allowed to promote all final quantitative claims.

## Requirement traceability

| Task | Requirements and criteria closed |
|---|---|
| T001 | FR-008, FR-014; enables SC-002 and SC-006 |
| T002 | FR-001--FR-007, FR-015, FR-017; SC-001 and the prose portion of SC-008 |
| T003 | FR-008, FR-009, FR-013, FR-014; SC-002 and deterministic portion of SC-006 |
| T004 | FR-006, FR-007, FR-010, FR-011, FR-014; SC-003 and SC-004 |
| T005 | FR-012--FR-016; SC-005--SC-007 |
| T006 | FR-008--FR-014; network-path confirmation for SC-002--SC-006 |
| T007 | FR-005, FR-007, FR-014--FR-017; SC-001 and SC-006--SC-008 |
| T008 | Presentation constraint only; preserves the T007 claim/evidence closure while enforcing ten body pages plus a separate References page |

## Parallel Example

After T001, paper restructuring in T002 and correctness-harness implementation in
T003 touch distinct primary files and can proceed independently. No other tasks
are parallelized because onboarding, overhead, and network confirmation consume
the preceding experiment contract and evidence.

## Implementation Strategy

1. Freeze claim and case contracts before changing prose or running experiments.
2. Land the paper's bounded architecture explanation with `TBD` tables.
3. Prove deterministic security semantics before measuring timing.
4. Resolve the onboarding epoch boundary before making the operational claim.
5. Measure marginal cost only after the security subject is stable.
6. Confirm network behavior, then admit results into the final paper.

## Fragmentation Scan

- Paper edits, citations, build, and claim-location updates remain one T002
  because none independently delivers a defensible contribution statement.
- Correctness tests, harness behavior, regression execution, and canonical
  evidence remain one T003 because they jointly prove the allow/deny contract.
- Onboarding instrumentation, any necessary minimal refresh fix, repetitions,
  and claim update remain one T004 because the implementation decision is
  controlled by the observed epoch transition.
- Overhead execution and paper replacement remain one T005 because unadmitted
  measurements have no independent paper value.

All tasks use the required checkbox, sequential ID, story label where applicable,
and concrete file paths.

---

## Phase 7: Convergence

- [x] T009 [US1] Close the retained four-Data name/signer evidence gap per FR-002 and SC-001 (partial) by first adding failing analyzer tests in `tests/python/test_authorization_evaluation.py`, then recording the actual validator-approved `SubscriptionData.packet` name, Producer prefix, sequence number, signer KeyLocator, and wire digest for Request, ACK, Selection, and Response receive paths in `ndn-service-framework/{ServiceUser,ServiceProvider}.cpp`; extend `Experiments/{run_authorization_evaluation,analyze_authorization_evaluation}.py` to run and validate one composed authorized MiniNDN transaction with TRACE/timeline capture, require all four producer/name/signer invariants under one request ID, retain a manifest-complete canonical artifact, run focused C++/Python and MiniNDN regressions, and promote `DC-1` in `contracts/claim-evidence-matrix.md` only if every gate passes.
