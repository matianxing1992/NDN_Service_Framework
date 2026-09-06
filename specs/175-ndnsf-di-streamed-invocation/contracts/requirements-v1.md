# Spec175 Active Requirements Registry

The active requirements are FR-001--FR-010 and SC-001--SC-005 in
[`../spec.md`](../spec.md). This file fixes their validation owners without
creating a second wording authority.

| Requirement | Validation owner | Closing evidence |
|---|---|---|
| FR-001--FR-004 | Generic stream lifecycle, wire/security, User and Provider production path | T020 audit plus T021 native/Python suites |
| FR-005--FR-007 | Qwen adapter, native epoch/state owner, conversation coordinator | T020 audit, T021 suites, T022 cold and continuation cases |
| FR-008 | Code-aware convergence gate | T020 `audit.md` PASS |
| FR-009 | Current-source complete local and MiniNDN qualification | T021 and T022 manifests |
| FR-010 | Explicit transfer with no expensive qualification claim | T023 handoff and closure record |

Spec175 owns no SIF, CUDA, TigerCluster, YOLO, throughput, or performance
success criterion. Historical requirements containing those obligations remain
provenance only under `evidence/archive/`.

