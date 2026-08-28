# Specification Quality Checklist: Pluggable DI Collaboration Planning

**Purpose**: Validate specification completeness and quality before planning

**Created**: 2026-07-28

**Feature**: [spec.md](../spec.md)

## Content Quality

- [x] No implementation details (languages, frameworks, concrete signatures)
- [x] Focused on researcher, application, and operator outcomes
- [x] Written so requirements can be reviewed independently of source layout
- [x] All mandatory sections completed

## Requirement Completeness

- [x] No `[NEEDS CLARIFICATION]` markers remain
- [x] Requirements are testable and unambiguous
- [x] Success criteria are measurable
- [x] Success criteria are technology-agnostic
- [x] All acceptance scenarios are defined
- [x] Edge cases are identified
- [x] Scope is clearly bounded
- [x] Dependencies and assumptions identified
- [x] Base NDNSF treats `ACK=true` generically; only a successfully validated,
  signed, versioned DI offer gives it request/attempt/model/boot/resource/
  deadline/expiry-bound willingness semantics
- [x] Exactly one immutable logical final-Selection identity per selected
  Provider directly and atomically assigns its complete
  role/fragment/artifact/dependency/resource tuple and consumes the existing
  ProviderToken once; byte-identical retries are idempotent and separately
  counted, with mandatory per-Provider acceptance evidence but no second
  role-negotiation round or global acceptance cover
- [x] Aggregate GPU RAM required by all roles assigned to one Provider cannot
  exceed the available GPU RAM offered or reserved in that Provider's ACK
- [x] ACK-time GPU-RAM promises are exclusive across concurrent requests until
  Selection, release, or expiry
- [x] Final Selection does not wait for complete all-role model readiness
- [x] Per-Provider Selection delivery is orthogonal to role execution, so one
  selected role may run while another Provider's Selection is still retried
- [x] Only final Selection installs recipient-encrypted per-role/per-input key
  grants, and its canonical digest binds every grant against tamper and
  cross-request/attempt/Provider/role replay
- [x] Role execution has one unambiguous gate: valid Selection, local
  readiness, and all authenticated direct inputs
- [x] Acceptance covers both input-before-model and model-before-input event
  orderings without duplicate execution
- [x] Public invocation, bounded attempt, Provider Selection delivery, role,
  object, resource, result, and Response lifecycles have distinct state
  machines and terminal rules
- [x] Provider Selection acceptance is crash-atomic across token/lease,
  NDNSF-DI admission ledger, tuple, grants, generation fence, and durable
  acceptance record through the specified generic Core WAL plus opaque
  participant mechanism; missing evidence is `UNKNOWN`
- [x] Output/result semantics cover object identity, lineage, schema/segment,
  digest, AEAD/signature/expiry, multiple sinks, and one accepted terminal
  result
- [x] Response success requires durable acceptance from every Provider in the
  result dependency closure and binds the required acceptance-set digest
- [x] Executable adapters/runners/publishers are explicit host-TCB members;
  signatures are not described as code sandboxing
- [x] Object confidentiality specifies nonce uniqueness or misuse-resistant
  AEAD, canonical AAD, public ciphertext digests, protected plaintext digests,
  and baseline NDN metadata leakage
- [x] Cancellation is local-terminal-CAS plus authenticated eventual
  convergence, not a cross-Provider atomic action
- [x] Replan creates a new attempt and cross-attempt reuse requires explicit
  `AdoptedInputEvidence`
- [x] Trust and non-goals are explicit: an external strategy is
  operator-trusted local code, signatures do not prove correct malicious
  computation, and no deadlock-freedom/distributed-atomicity claim is made
- [x] Base NDNSF versus NDNSF-DistributedInference ownership is explicit and
  legacy Core DI behavior is tracked as migration debt
- [x] The existing generic NDNSF Collaboration API is the only normative
  invocation carrier; Spec 163 does not introduce a parallel DI
  Request/Selection/data/status/Response lifecycle
- [x] `begin_collaboration`, immutable `ACK_CLOSED`, and `commit_plan` are
  generic local API/state boundaries rather than new wire messages or a second
  willingness/preparation round
- [x] Existing roles/dependencies-before-Request calls are explicitly
  `PREPLANNED` compatibility, while dynamic Spec 163 planning defaults to
  `DEFERRED` and forbids placeholder roles
- [x] Requester-side plan commit is distinguished from Provider-side Selection
  acceptance: the former consumes no ProviderToken and proves no Provider
  consent
- [x] History-oracle evidence is bounded and reproducible; property testing is
  not described as a general proof
- [x] The public application request is model-family-neutral and binds
  `ModelRef`, task, typed input/options, deadline, objective, and constraints;
  it does not require deployment, Provider roles, or LLM-only fields
- [x] `ModelFamilyAdapter` is composed from graph/split, task-I/O, state, and
  runner ports; pure ports receive no network/repository/Selection/device
  authority, and opaque unsplittable models can use one atomic graph node
- [x] Mutable inference state is classified explicitly; absence of a reusable
  `InferenceStateContract` means terminal destruction
- [x] Exact prefix KV identity binds model/semantics, adapter/runner,
  split/layer, prefix/position, precision/layout, security domain,
  Provider/epochs, and expiry; ACK exposes only bounded opaque evidence
- [x] State reuse is sealed into plan and Selection, pinned/revalidated before
  use, denied across tenants by default, and falls back cleanly on mismatch
- [x] Derived state is Provider-local by default and is not published as an
  immutable model shard; optional encrypted migration has a separate bounded
  contract and is not a baseline gate

## Feature Readiness

- [x] All functional requirements have clear acceptance criteria
- [x] User scenarios cover the primary planning, extension, preparation, and
  execution flows
- [x] Request-level lifecycle and per-role readiness are not forced into one
  global linear phase
- [x] Measurable outcomes and their future evidence gates are defined; this
  design review does not claim implementation has met them
- [x] No concrete implementation signature leaks into the specification

## Notes

- Scope authority is the dissertation proposal
  `docs/PAPER/proposal-defense/main_ch.pdf`, not the broader project proposal
  `docs/PAPER/reference-pdfs/ai_proposal.pdf`.
- Public type names, method signatures, state-machine messages, migration
  mechanics, and source ownership are intentionally deferred to `plan.md`,
  `data-model.md`, and `contracts/`.
- The prior complete-`PreparedSet` gate and a second role-negotiation round are
  removed. The controlling semantics are signed DI offers followed directly by
  exact final Selection assignments, mandatory per-Provider acceptance
  evidence for crash-safe retry, and independent role-local preparation and
  input latches for execution.
