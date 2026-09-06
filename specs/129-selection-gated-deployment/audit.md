# Spec 129 Pre-Implementation Audit

> **Superseded audit notice (2026-07-21):** This file audits R0's advisory ACK
> and complete ReadySet activation design. Spec 129 R1 replaces both contracts.
> The current audit is [audit-r1.md](audit-r1.md); do not use the verdict below
> as R1 implementation authorization.

## Verdict

`PASS`

The feature is ready to begin T001. Requirements, design, contracts, migration,
tasks, validation, and code reality align with no unresolved CRITICAL or HIGH
finding. PASS means the plan is executable; it does not claim that any feature
behavior is implemented, wired, executed, or measured.

## Findings

No unresolved findings.

Two issues found during the audit were remediated before this final verdict:

1. Traceability used requirement ranges that the deterministic scanner could
   not expand; `traceability.md` now maps every FR explicitly and the strict
   scan reports 28/28 traced.
2. Initial planning treated `DeploymentRevision` and
   `ExecutionCommitCertificate` as slide-only terminology. CodeGraph proved
   they are live persisted/runtime contracts. The final plan now migrates their
   journal, coordinator, native gate, client/provider, cancellation, example,
   test, and compatibility surfaces instead of introducing parallel authority.

## Code-Reality Evidence

| Claim | Current source fact | Planned closure |
|---|---|---|
| Status query authentication | `ServiceUser::QuerySelectionStatus` constructs an exact MustBeFresh Interest but does not sign/authenticate its parameters (`ndn-service-framework/ServiceUser.cpp:1377-1395`). | T001/T006 add signed requester-bound query and replay handling. |
| Status confidentiality | Provider serializes status directly into Data content, then signs it (`ndn-service-framework/ServiceProvider.cpp:2751-2766`). | T001/T006 add requester-scoped AEAD envelope, key wrapping, and plaintext-negative gates. |
| Existing status security routing | `GetAttributesByName` covers REQUEST/ACK/Selection/Response but returns no mapping for Selection-status (`ndn-service-framework/utils.cpp:649-670`). | T001/T006 define explicit secure-status naming/authorization without weakening existing routes. |
| Persisted revision authority | APPDeployment stores/restores `DeploymentRevision`, state, definition, epoch, and readiness bindings (`NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/deployment.py:65-77,136-160`). | T003 supplies v1 read/v2 plan-instance rewrite, restart, rollback, malformed-record, and no-dual-writer gates. |
| Existing readiness verification | APPDeployment already validates exact roles, plan/revision binding, artifact digests, boot epoch, expiry, evidence digest, signer, and readiness (`NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/deployment.py:289-322`). | T004 reuses this authority as the one ProviderReady verifier/state store and adds network delivery. |
| Existing execution certificate | `ExecutionCommitCertificate` validates exact assignments and bound commit receipts (`NDNSF-DistributedInference/ndnsf_distributed_inference/core/contracts.py:324-358`). | T005 migrates it to ExecutionActivateMessage plus ReadySet summary/digest; no second execution gate. |
| Native certificate gate | `NativeProviderHandlerConfig` exposes `requireExecutionCommitCertificate` (`NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp:32-41`). | T005 migrates the native gate/config with a bounded deprecated reader and telemetry. |

## Traceability Gaps

None. All 28 functional requirements and all 12 success criteria map to tasks
and intended tests/evidence in `traceability.md`.

## Readiness Scorecard

| Dimension | Ready? | Notes |
|---|---|---|
| Intent and scope | Yes | Captures all clarified slide changes; Spec 128 remains frozen. |
| Architecture and ownership | Yes | Core owns generic wire/security/barrier; DI owns model planning and RunnerAdapter preparation. |
| Security/correctness | Yes | Auth, confidentiality, replay, fencing, bounded retries, restart, partial failure, and fail-closed paths are explicit. |
| Task executability | Yes | Ten ordered tasks name concrete files, behavior, negative cases, and acceptance gates. |
| Task cohesion/granularity | Yes | Test-first work, implementation, focused validation, and evidence are coalesced by behavior; no mechanical fragments. |
| Validation/evidence | Yes | Deterministic, migration, full build/binding, compatibility/security, and fresh 12-cell MiniNDN gates are specified. |
| Migration/rollback | Yes | One canonical state machine, v1 read/v2 write, isolated deprecated shims, negotiation, rollback, and later deletion gate are explicit. |
| Code reality | Yes | Live plaintext status, revision persistence, receipt coordination, certificate callers, and native gate are reflected in tasks. |

## Metrics

- User stories: 5
- Functional requirements: 28
- Success criteria: 12
- Tasks: 10
- Mechanically fragmented task groups: 0
- Coalescing opportunities: 0
- Requirement coverage: 100% (28/28)
- Unmapped tasks: 0
- Placeholders/clarifications: 0
- Critical / High / Medium / Low unresolved findings: 0 / 0 / 0 / 0

## Assumptions and Evidence Limits

- This is a pre-implementation audit. No implementation/build/test/MiniNDN
  result is claimed for Spec 129.
- CodeGraph was current at audit time, but unknown external Python/C++ callers
  cannot be proven absent. The plan therefore keeps isolated deprecated
  read/import/config shims for a bounded compatibility window.
- The worktree already contains many user changes. Implementation must snapshot
  source hashes and avoid reverting or normalizing unrelated files.
- The configured Spec Kit agent-context extension script is absent from the
  repository. `AGENTS.md` was updated manually to the Spec 129 plan; the missing
  hook implementation should be repaired separately if automatic hooks are
  required later.

## Next Actions

1. Begin T001 only: freeze versioned wire/name/crypto contracts and make their
   negative tests fail before runtime integration.
2. Proceed sequentially through T002-T007 because these tasks share authority,
   state, wire, and binding files.
3. Do not launch MiniNDN until T009 runner/preflight passes; T010 is the only
   task authorized to execute the frozen 12-cell campaign.
