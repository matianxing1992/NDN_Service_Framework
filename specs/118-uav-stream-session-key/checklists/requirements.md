# Specification Quality Checklist: UAV Stream Session-Key Delivery

**Purpose**: Validate specification completeness and quality before planning

**Created**: 2026-07-17

**Feature**: [spec.md](../spec.md)

## Content Quality

- [x] Focuses on UAV Mapping/payload security while assigning generic Mapping ownership to Spec 119.
- [x] Separates stakeholder requirements from implementation choices reserved for the plan.
- [x] Keeps generic NDNSF Response outside scope and forbids a second Mapping RPC path.
- [x] All mandatory sections are complete.

## Requirement Completeness

- [x] No `[NEEDS CLARIFICATION]` markers remain.
- [x] Requirements are testable and unambiguous.
- [x] Success criteria are measurable and observable.
- [x] Acceptance scenarios and security-negative cases are defined.
- [x] Edge cases, scope, dependencies, and assumptions are explicit.

## Feature Readiness

- [x] Every functional requirement has an acceptance path.
- [x] Stories cover secure Mapping/start/consume, rejection, and rotation.
- [x] The design preserves service-level sharing and states its secrecy limits.
- [x] Cross-Spec ordering requires Spec 119 T001-T004 before UAV runtime integration.
- [x] Cross-session explicit-name uniqueness, five distinct frontiers, routed
  prefix lifecycle, exact trust relation, and one-time ciphertext materialization
  are normative rather than left to implementation judgment.
- [x] Successful start requires bounded measured readiness and an APP-proven,
  Mapping-covered decoder-safe join; equivalent start returns a fresh snapshot.
- [x] Retained payload and complete Mapping-chain availability advance together,
  and LiveStream-handle pending limits are not misrepresented as NFD PIT enforcement.
- [x] A dedicated UAV Stream trust configuration, deployment check, and wrong-
  valid-Provider rejection have explicit implementation owners.
- [x] Encryption-before-Core, optional ciphertext FEC, and decryption-after-Core
  are normative; no duplicate UAV XOR owner remains in the target design.

## Notes

- The exact Mapping, cipher envelope, nonce construction, field serialization, and
  source paths belong in `plan.md` and the contracts.
- Existing negative FEC results remain unchanged; this feature does not claim
  reliability improvement.
