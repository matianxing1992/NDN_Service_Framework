# Feature Specification: Data-Centric Authorization Evaluation

**Feature Directory**: `specs/172-data-centric-authorization-evaluation`

**Created**: 2026-08-11

**Status**: In progress

**Input**: Revise the NDNSF paper to explain and evaluate its data-centric service transaction model and its separation of identity authentication from attribute-based service authorization.

## User Scenarios & Testing

### User Story 1 - Defensible security and data-centric contribution (Priority: P1)

As a paper reader, I can distinguish NDNSF's framework contribution from the pre-existing NDN, trust-schema, and NAC-ABE primitives on which it is built.

**Why this priority**: The paper cannot make a credible novelty claim or interpret experiments until it states exactly what NDNSF contributes.

**Independent Test**: A reviewer can map every security and data-centric contribution claim to a protocol invariant, implementation behavior, prior-work distinction, or explicit limitation without inferring missing steps.

**Acceptance Scenarios**:

1. **Given** the revised abstract, introduction, background, design, related work, and conclusion, **when** a reader asks what is new, **then** the paper identifies a uniform producer-scoped Request--ACK--Selection--Response Data transaction and service-semantic authorization as framework contributions, not new cryptography.
2. **Given** an authenticated message producer, **when** the paper explains service authorization, **then** it separately identifies signature/trust validation, service-scoped attribute authorization, controller permission state, and one-time transaction tokens.
3. **Given** comparisons with MF-IoT, DNMP, NSC, and NAC-ABE, **when** the paper describes differences, **then** each statement is supported by a primary source and does not claim that prior frameworks are incapable of adopting a similar design.

---

### User Story 2 - Reproducible authorization evidence (Priority: P1)

As a researcher, I can run a focused experiment suite that demonstrates authorized success, unauthorized denial, stale-policy rejection, signer/name mismatch rejection, and token replay rejection across the service transaction.

**Why this priority**: Unit mechanisms alone do not support a paper-level claim that the composed authorization path works end to end.

**Independent Test**: A fresh run produces a manifest and per-case results in which every registered allow/deny case reaches its expected terminal state and no denied case executes the service handler.

**Acceptance Scenarios**:

1. **Given** correctly provisioned User and Provider identities, service rights, policy epoch, and fresh tokens, **when** a request completes, **then** exactly one authorized execution and one accepted response are recorded.
2. **Given** a missing User or Provider service right, **when** the same request is attempted, **then** it is denied before service execution and the denial stage is recorded.
3. **Given** a stale epoch, mismatched signer/name relation, mismatched token, or replayed token, **when** the message is processed, **then** the expected security gate rejects it and records zero new executions.

---

### User Story 3 - Provider-decoupled User onboarding evidence (Priority: P1)

As a paper author, I can state precisely whether a newly authorized User can use an existing Provider without a per-User Provider configuration or trust-chain change, while still reporting any automatic policy refresh that is required.

**Why this priority**: The strongest proposed authorization benefit depends on separating manual Provider reconfiguration from protocol-level policy refresh.

**Independent Test**: An existing Provider's executable, service configuration, identity, and trust configuration remain byte-identical while a new User is provisioned; after the Provider reaches the required policy epoch, the new User completes its first authorized request.

**Acceptance Scenarios**:

1. **Given** an initialized Provider that is unavailable during new-User provisioning, **when** the Provider reconnects with unchanged local configuration and refreshes controller-issued policy material, **then** the new User can invoke the existing service.
2. **Given** a Provider that retains an older policy epoch, **when** the new User sends a request with the current epoch, **then** the request is rejected and the experiment reports the refresh dependency rather than claiming zero Provider interaction.
3. **Given** the same onboarding scenario under an identity- or ACL-coupled comparison configuration, **when** provisioning work is counted, **then** the comparison reports manual configuration changes, control messages, state changes, and time to first successful invocation separately.

---

### User Story 4 - Authorization cost and scaling evidence (Priority: P2)

As a system evaluator, I can quantify the marginal cost of NDNSF authorization without conflating it with Sync, service execution, mobility, or Provider selection.

**Why this priority**: A framework contribution must report both functional benefit and cost.

**Independent Test**: Matched runs report cold and warm authorization cost, wire overhead, controller provisioning work, and end-to-end latency using identical service and network conditions.

**Acceptance Scenarios**:

1. **Given** matched secured and comparison runs, **when** the authorization path is measured, **then** the results separate key acquisition, ABE key wrapping/unwrapping, symmetric payload encryption, signature validation, and application execution.
2. **Given** increasing Users, Providers, services, and policy complexity, **when** provisioning and invocation are measured, **then** the experiment reports state size, bytes, operations, latency distributions, and failures rather than only averages.
3. **Given** preliminary paper tables, **when** a measurement has not completed its registered verification, **then** the cell remains `TBD` and is explicitly labeled as a planned result.

### Edge Cases

- Adding a User changes the global policy epoch even when Provider service rights are unchanged.
- A Provider is offline during policy modification and reconnects with stale cached permissions.
- An identity is valid under the trust schema but lacks the service attribute or controller permission.
- An entity has the correct service attribute but signs a message under a name inconsistent with its identity.
- A previously valid UserToken or ProviderToken is replayed under either the original or a different request ID.
- The Attribute Authority or Service Controller is unavailable during cold onboarding but cached authorization material remains usable for an existing policy epoch.
- The ABE backend is replaced in principle, while the evaluated implementation remains NAC-ABE-specific.
- A result table exists before the corresponding experiment has produced complete evidence.

## Requirements

### Functional Requirements

- **FR-001**: The paper MUST use `data-centric service transaction` rather than ambiguous `data-driven service framework` terminology when describing the protocol contribution.
- **FR-002**: The paper MUST define the producer-scoped Request, ACK, Selection, and Response Data invariant and explain how Sync announces the objects for retrieval.
- **FR-003**: The paper MUST distinguish identity authentication, service authorization, confidentiality, controller permission state, and transaction/replay binding.
- **FR-004**: The paper MUST state that NAC-ABE is an adopted cryptographic building block and MUST NOT claim new ABE cryptography.
- **FR-005**: The paper MUST compare NDNSF with primary-source descriptions of NAC/NAC-ABE, MF-IoT, DNMP, and NSC, and MUST bound any `first` claim to the reviewed evidence.
- **FR-006**: The paper MUST state that adding an authorized User avoids per-User Provider ACL or trust-chain configuration, while disclosing any policy-epoch refresh required by the current implementation.
- **FR-007**: The paper MUST NOT claim fully automated onboarding, zero Provider interaction, complete revocation, or trust-chain elimination unless a corresponding verified experiment and implementation exist.
- **FR-008**: The evaluation MUST include a registered authorization correctness matrix covering authorized success, missing User permission, missing Provider permission, stale epoch, signer/name mismatch, token mismatch, and token replay.
- **FR-009**: Every denied authorization case MUST prove that the service handler did not execute.
- **FR-010**: The evaluation MUST include a new-User/existing-Provider scenario that preserves Provider-local configuration hashes and separately measures automatic policy refresh.
- **FR-011**: The onboarding comparison MUST separate manual configuration changes, control-plane messages, state changes, onboarding latency, and time to first successful invocation.
- **FR-012**: The evaluation MUST report cold and warm authorization overhead, latency distributions, wire bytes, cryptographic operation counts, and failure counts.
- **FR-013**: Experimental comparisons MUST keep the NDNSF service transaction, workload, payload, and network conditions matched so that the authorization mechanism is the intended independent variable.
- **FR-014**: Every formal run MUST retain a manifest containing source hashes, configuration, exact command, environment, terminal status, and result hashes.
- **FR-015**: Preliminary tables MUST use visibly labeled `TBD` cells and MUST NOT be described as measured results.
- **FR-016**: Paper claims MUST be replaced with measured values only after the corresponding evidence passes focused regression, reproduction, and claim-to-result checks.
- **FR-017**: Existing mobility and distributed-inference evidence MUST remain unchanged unless a security experiment directly requires a documented integration change.

### Key Entities

- **Identity authentication policy**: Rules connecting a Data name, signer key, certificate chain, and trust anchor.
- **Service authorization policy**: Controller-managed right for a User to invoke or Provider to offer a hierarchical service name.
- **Service attribute**: Authorization attribute associated with Request and Selection access.
- **Permission attribute**: Authorization attribute associated with ACK and Response access.
- **Permission response**: Controller-signed, target-encrypted authorization state carrying a policy epoch.
- **Transaction token**: One-time UserToken or ProviderToken binding messages to a request and selection decision.
- **Authorization case**: Registered initial state, action, expected gate, expected execution count, and observed evidence.
- **Experiment manifest**: Immutable description of subject, command, environment, configuration, source hashes, outputs, and terminal status.

## Success Criteria

### Measurable Outcomes

- **SC-001**: Every paper contribution claim is mapped to at least one current implementation invariant and one prior-work distinction or explicitly marked as a limitation.
- **SC-002**: The authorization matrix completes all registered cases with 100% agreement between expected and observed allow/deny outcomes and zero handler executions in every denied case.
- **SC-003**: The onboarding experiment completes at least three independent repetitions in which Provider-local executable, service configuration, identity, and trust configuration hashes remain unchanged.
- **SC-004**: The onboarding report separately quantifies manual Provider changes, automatic refresh operations, control bytes, onboarding latency, and time to first successful invocation for every compared configuration.
- **SC-005**: The overhead study reports median, p95, and maximum latency; operation counts; wire bytes; and failure counts for both cold and warm paths across every registered scale point.
- **SC-006**: Repeating a deterministic authorization case yields identical terminal outcome, execution count, and evidence hashes; repeated timed measurements retain all run-level observations and report their variation.
- **SC-007**: No `TBD` result is cited as evidence, and every numerical paper claim resolves to a retained canonical result artifact.
- **SC-008**: The revised paper builds successfully and a claim audit finds no statement that equates decryption with identity authentication, eliminates trust chains entirely, or overstates current onboarding/revocation support.

## Assumptions

- The paper remains a systems paper; it does not claim a new ABE construction or provide a new cryptographic security proof.
- NAC-ABE is the evaluated authorization backend, although the framework contract may admit alternatives.
- The Service Controller and Attribute Authority are trusted for policy correctness and key issuance.
- MF-IoT is used as a conceptual ICN comparison, not as a directly matched NDN performance baseline.
- A same-framework authorization ablation is preferred for causal overhead comparison; NSC and DNMP remain architectural comparisons unless an equivalent runnable security configuration is available.
- Revocation results are out of scope until the implementation exposes a complete, testable revocation lifecycle.
- Existing dirty worktree changes belong to ongoing project work and are not to be reverted or reformatted by this feature.

## Out of Scope

- Designing a new ABE algorithm or proving the cryptographic security of NAC-ABE.
- Reimplementing MF-IoT on MobilityFirst solely as a performance baseline.
- Replacing or rerunning the active mobility campaign unless needed for a specific security integration check.
- Claiming malicious service-computation correctness; this feature covers authorization to participate, message authenticity, transaction binding, and replay rejection.
