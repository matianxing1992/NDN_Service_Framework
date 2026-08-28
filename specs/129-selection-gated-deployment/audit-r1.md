# Spec 129 R1 Pre-Implementation Audit

**Date**: 2026-07-21; second adversarial pass incorporated  
**Scope**: `spec.md`, `plan.md`, `research.md`, `data-model.md`, contracts,
`quickstart.md`, `tasks.md`, `traceability.md`, requirement checklist, and
current source reality through CodeGraph.

## Verdict

`PASS`

Spec 129 R1 is internally consistent and ready to begin T001. PASS authorizes
implementation from the open task list; it does not claim that R1 is
implemented, built, tested, or confirmed in MiniNDN.

## Remediated Findings

| ID | Severity | Location | Finding | Resolution |
|---|---|---|---|---|
| R1-A1 | HIGH | `spec.md:256-263` | Broad positive-ACK wording could impose exclusive reservation on ordinary NDNSF applications. | Reservation, tombstone, and mandatory negative Selection now apply only to explicitly negotiated `DIReservationSelectionV1`; non-DI positive ACK behavior is a required compatibility regression. |
| R1-A2 | HIGH | `spec.md:359-376` | Input confidentiality was coupled to DI reservation despite being reusable across applications. | Added independently negotiated generic `SelectionGatedInputV1`; REQUEST, ACK, and `NOT_SELECTED` never disclose its input key. |
| R1-A3 | HIGH | `spec.md:289-293` | A higher-sequence conflicting Selection could reverse an earlier decision. | First valid Selection is immutable; only same-digest duplicates are idempotent, and every conflict is rejected regardless of sequence. |
| R1-A4 | HIGH | `contracts/deployment-control-contract.md:24-32` | Reservation could be allocated before authorization was proven. | Provider authenticates and authorizes Requester/service/DI intent before atomic reservation. |
| R1-A5 | MEDIUM | `spec.md:268-272` | Selected commit near tentative expiry lacked an atomic boundary. | Commit must complete before tentative expiry, cannot resurrect expired state, and creates a bounded committed execution lease. |
| R1-A6 | MEDIUM | `spec.md:318-323` | Contention retry did not consistently require all partial reservations to be gone. | USER waits for every release receipt or lease expiry before backoff/new attempt. |
| R1-A7 | MEDIUM | `spec.md:372-375` | Input-key lifetime and persistence rules were incomplete. | Unwrapped keys cannot be durably persisted and have bounded erasure conditions. |
| R1-A8 | MEDIUM | `quickstart.md:44-50` | Quickstart and tasks left focused test artifacts unnamed. | Added concrete commands and exact test/regression paths. |
| R1-A9 | HIGH | `data-model.md:3-12,53-77` | The first capability split was not wire-realizable: input grants still required DI fields. | Added generic Request capability metadata, non-reserving `SelectionInputKeyOffer`, and input-only `SelectionInputKeyGrant`. |
| R1-A10 | HIGH | `spec.md:337-342` | Selection-gated disclosure had no behavior on the Selection-free Targeted fast path. | Incompatible calls fail before publication; ordinary Targeted remains unchanged. |
| R1-A11 | MEDIUM | `contracts/deployment-control-contract.md:33-39` | Generic ACK policy could be assumed to inspect plaintext despite encrypted input. | Negotiated providers decide from authenticated metadata/ciphertext and advertise a certificate only on successful ACK. |

No unresolved CRITICAL, HIGH, MEDIUM, or LOW finding remains in the design
artifacts.

## Code-Reality Evidence

| Current source fact | R1 consequence |
|---|---|
| `ServiceUser::PublishServiceSelectionMessageV2` accepts `providerName`, but publishes a compact Selection name without the Provider component and constructs an R0 `DeploymentPlan`. | T001/T003 must add versioned exact-target transport without silently changing non-negotiated callers. |
| Existing compact Selection may carry Provider entries/assignment payloads in one group-protected message. | T003/T004 must provide one recipient-confidential decision per R1 reservation and preserve generic compatibility. |
| Current R0 deployment handling uses advisory `ProviderCapabilityOffer`, ReadySet, and activation authority. | T002/T005/T006 are migrations; no current implementation claim is made. |
| Existing generic ACK handlers may inspect `RequestMessage`. | `SelectionGatedInputV1` remains opt-in rather than an unconditional payload rewrite, preserving applications that need input-dependent ACK policy. |
| `RequestMessage` currently has payload/mode/target fields but no generic capability set. | T001 adds capability metadata at the generic message layer rather than embedding input-only negotiation in DI `DeploymentIntent`. |
| `RequestServiceTargeted` uses cached tokens to skip ACK/Selection and otherwise enters `TargetedBootstrapRequest`; `generic-dynamic-api-targeted.t.cpp` exercises both paths. | T001/T004 add a pre-publication incompatibility test and retain the existing Targeted suite as compatibility evidence. |

CodeGraph was current during audit: 2,579 files, 55,457 nodes, 180,675 edges.

## Readiness Scorecard

| Dimension | Result | Basis |
|---|---|---|
| Intent fidelity | PASS | Latest ACK scope and independent input-confidentiality decisions are explicit. |
| Necessity/Occam | PASS | No controller, consensus, UAV, codec, model-family, or workload special case was added. |
| Ownership | PASS | Core owns optional generic wire/security; NDNSF-DI owns reservations, resources, DAG, preparation, and retry policy. |
| Security | PASS | Authorization-first reserve, recipient encryption, immutable decision, fencing, replay failure, key lifetime, and fail-closed migration are specified. |
| Failure/recovery | PASS | Loss, delay, late ACK, restart, expiry, conflict, cancellation, partial acquisition, and exhaustion have bounded outcomes. |
| Migration/rollback | PASS | Two capabilities negotiate independently; absent capabilities preserve existing behavior; R1 leases expire safely on rollback. |
| Task executability | PASS | Eleven dependency-ordered tasks name concrete files, tests, and acceptance conditions. |
| Evidence design | PASS | Deterministic, compatibility, security, build/binding, and fresh exact-once MiniNDN gates are defined without reusing R0 evidence. |

## Coverage Metrics

- User stories: 5
- Functional requirements: 33
- Success criteria: 13
- Open tasks: 11
- Requirement traceability: 33/33 (100%)
- Placeholders or unresolved clarifications: 0
- Unresolved findings: 0

## Evidence Limits

- This audit changed specification artifacts only.
- No C++/Python implementation, build, binding rebuild, regression, or MiniNDN
  cell was executed.
- Current R0 source behavior remains until T001-T009 are implemented and
  verified. R0 and Spec 128 experiment evidence remains untouched.
- The worktree contains unrelated user changes; implementation must remain
  scoped and must not normalize or revert them.

## Authorized Next Step

Begin T001 by freezing the two independent capability contracts and making the
wire, malformed-input, security, compatibility, and four-combination tests fail
before runtime implementation. Do not launch the live matrix before T010's
single-writer preflight and all deterministic gates pass.
