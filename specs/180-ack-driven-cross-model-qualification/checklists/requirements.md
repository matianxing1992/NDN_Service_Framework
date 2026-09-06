# Specification Quality Checklist: ACK-Driven YOLO and Cross-Model Qualification

**Purpose**: Validate specification completeness before planning
**Created**: 2026-09-02
**Feature**: [spec.md](../spec.md)

## Content Quality

- [x] Scope and user value are explicit.
- [x] Model/runtime names appear only where they define the qualification subject.
- [x] All mandatory sections are complete.
- [x] Existing implementation is reused instead of described as a greenfield system.

## Requirement Completeness

- [x] No `[NEEDS CLARIFICATION]` markers remain.
- [x] Requirements are testable and unambiguous.
- [x] Success criteria are measurable.
- [x] Acceptance scenarios and edge cases are defined.
- [x] Functional qualification is separated from performance claims.
- [x] Security, failure, cleanup, and evidence boundaries are explicit.
- [x] Large invocation input has one explicit repository-reference transport,
      authorization, confidentiality, fetch-owner, and redaction contract.
- [x] Candidate feasibility is candidate-local and independent of catalogue order.
- [x] Multiple feasible candidates use signed adapter priority followed by a
      stable digest tie-break.
- [x] Local CPU reference evidence is explicitly separated from Tiger CUDA
      and no-CPU-fallback evidence.
- [x] Complete local suites run in supervised process-isolated children.
- [x] The local suite/case inventory is explicit and source-bound; no unrecorded
      subset may be called a complete gate.
- [x] Pipeline/rank and ONNX component-set roles have distinct wire validation.
- [x] Immutable candidate and change invalidation are required.
- [x] SIF qualification forbids host-built extensions and records the active
      in-image Python/ABI/import/library closure.
- [x] Scope, dependencies, assumptions, and exclusions are explicit.
- [x] Candidate catalogue trust, external model-manifest trust, Merge ownership,
      and frozen Qwen reference ownership are explicit, including separate
      configured signer roots.

## Feature Readiness

- [x] Every user story has an independent test.
- [x] Every functional requirement has an observable acceptance path.
- [x] YOLO implementation and cross-model qualification have a fixed dependency order.
- [x] Spec175 handoff is an explicit prerequisite for formal qualification.

## Notes

Validation iteration 54: PASS for specification completeness and T001's
fail-closed implementation gate after code-aware audit corrections. The
catalogue trust root registry, distinct external model-manifest authority,
opaque pre-ACK catalogue lifecycle, Q-W valid/negative sequence,
repository-root task paths, and signed QWEN-F 27B manifest gate are explicit.
No clarification marker remains. The registry is configured with checked-in
Ed25519 public-key identities; this authorizes later verification but is not a
model export, deployment, or qualification result. The canonical YOLO role
names are `DetectShard0/1`; bare DetectShard roles are component sets, while
explicit stage/tensor/rank namespaces remain tensor-rank roles. Negative
signed priorities are rejected before candidate enumeration.
Detailed implementation choices and execution commands belong in `plan.md`,
contracts, and `tasks.md`. The current audit also requires the production
Provider-offer verifier to use the candidate-bound NDNSF Trust Schema anchor;
the caller HMAC map remains fixture-only.

Iteration 50 additionally records the implemented ServiceUser/pybind/Python
projection of validated ACK signer identity, KeyLocator, and full-wire digest.
This is focused implementation evidence only; production offer verification,
live ACK-to-Response execution, convergence, and qualification remain open.

Iteration 52 additionally records the explicit native `trustSchemaValidated`
ACK provenance marker and the maintained YOLO migration to the candidate-bound
Ed25519 policy/key map. These are focused source and regression results; the
real certificate-chain callback and ACK-to-Response runner remain required
before qualification.
The V3 configuration boundary now rejects caller-supplied ACK-coverage roles
and predicates, preserving the registered timeout as the only qualification
closure authority; V2 compatibility hooks remain unchanged. This is a focused
guard, not evidence that the live NDN path has executed.

The T002 generic transport boundary is intentionally not marked complete until
post-Selection ingress-only fetch/decrypt and terminal result-egress ownership
are exercised. The maintained YOLO example now defaults to the model-first
`request_task()` path; legacy service-policy calls require `--offline-oracle`.
The maintained `REPO_REF` path must also publish the encoded input through the
canonical `APPClient.publish_application_input_reference()` call before
`REQUEST_SENT`; reading a reference JSON file alone is not publication
evidence. The helper and native binding now perform publication and source-bound
metadata validation, but this remains an explicit T002/T009/T011 blocker until
the live ACK/Selection/Provider/Response path is exercised.
The current focused suite and resolver tests are implementation evidence only;
they do not prove live NDN publication, ACK/Selection, Provider execution,
MiniNDN qualification, SIF replay, or Tiger execution. T010 and T013 remain
partial; T011 and T014--T020 remain unchecked until those paths produce
candidate-bound evidence and T014 returns a fresh convergence `PASS`.
The current `ciphertextDigest` wire key is documented as a compatibility alias
for the post-decryption plaintext digest; it is not an encrypted-segment wire
digest.
