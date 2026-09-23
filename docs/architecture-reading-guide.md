# Architecture Reading Guide

This guide defines the minimum architecture reading order for repository work.
It keeps task execution anchored in ownership and production call paths before
implementation or expensive validation begins.

## Reading order

0. Read [`Design/highlevel-design.md`](../Design/highlevel-design.md) first. Map
   the proposed change to its shared and module principles, preserve the stated
   ownership boundaries, and record evidence for compliance. Existing gaps are
   repair work, not permission to weaken a principle; changes to principles
   require explicit user acceptance.
1. Read [`architecture.md`](architecture.md) for the framework/application
   ownership map and the accepted runtime direction.
2. Read [`ndnsf-core-app-boundary.md`](ndnsf-core-app-boundary.md) when a change
   crosses Core, DistributedInference, DistributedRepo, or UAV responsibilities.
3. Read [`NDNSF-DI-runtime-workflow.md`](NDNSF-DI-runtime-workflow.md) for
   build/runtime boundaries, candidate identity, launch barriers, cleanup, and
   result hygiene.
4. Read [`.specify/memory/design-code-convergence.md`](../.specify/memory/design-code-convergence.md)
   before formal validation, and [`.specify/memory/context-mode.md`](../.specify/memory/context-mode.md)
   whenever Context Mode retrieval or authority is involved.
5. Read the active feature selected by `.specify/feature.json`: `spec.md`,
   `plan.md`, `tasks.md`, `traceability.md`, the applicable contracts, and the
   newest evidence/failure record. The active Spec controls task order and
   acceptance claims.
6. Use CodeGraph to verify the real public entry point, production callers,
   security/configuration owner, and evidence-producing path before editing
   indexed source.

## Key architecture documents

| Concern | Read first | Authority question |
| --- | --- | --- |
| Core service lifecycle, permissions, ACK/Selection/Response | [`architecture.md`](architecture.md), [`ndnsf-core-app-boundary.md`](ndnsf-core-app-boundary.md) | Which layer owns the behavior and wire contract? |
| DI planning, role ownership, dependency dataflow | [`architecture.md`](architecture.md), active Spec contracts, CodeGraph symbols | Which planner/runtime owner creates and consumes the edge? |
| Native provider execution and exact data transfer | [`NDNSF-DI-runtime-workflow.md`](NDNSF-DI-runtime-workflow.md), active plan | Which source/build/runtime identity is being exercised? |
| Convergence, test, and evidence gates | [`design-code-convergence.md`](../.specify/memory/design-code-convergence.md), active `audit.md` and `tasks.md` | Is the subject ready for formal validation, or only focused repair? |
| Context retrieval and checkpoint recovery | [`context-mode.md`](../.specify/memory/context-mode.md), `.specify/feature.json`, active Spec evidence | Is the result repository-authoritative and current? |
| Failure diagnosis and retry order | [`failure-log.md`](failure-log.md), linked active-Spec evidence | What failed first, what was invalidated, and what is allowed next? |

## Spec180 reading set

For the current Spec180 vertical slice, the minimum set is:

```text
docs/failure-log.md
docs/architecture-reading-guide.md
docs/architecture.md
docs/NDNSF-DI-runtime-workflow.md
.specify/memory/design-code-convergence.md
specs/180-ack-driven-cross-model-qualification/spec.md
specs/180-ack-driven-cross-model-qualification/plan.md
specs/180-ack-driven-cross-model-qualification/tasks.md
specs/180-ack-driven-cross-model-qualification/traceability.md
specs/180-ack-driven-cross-model-qualification/contracts/
specs/180-ack-driven-cross-model-qualification/audit.md
specs/180-ack-driven-cross-model-qualification/evidence/<newest relevant record>
```

Do not use this list to bypass the active pointer or to treat an old evidence
file as current. It is a reading order; the active Spec and current source
remain authoritative.
