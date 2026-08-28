# Traceability: iTiger Qwen3.6 Generation

| Requirement | Design/contract | Task | Validation/evidence |
|---|---|---|---|
| FR-001, FR-002 | `research.md` Decisions 1/5; `contracts/model-and-placement.md` | T001, T002 | immutable model/runtime manifest; prompt/reference manifest |
| FR-003, FR-004, FR-004a | placement contract; `data-model.md` StageArtifact | T004, T005 | three distinct node/GPU receipts; exact layer union; reference layer map equals frozen stage cuts |
| FR-005 | `plan.md` RTX-only architecture | T003, T004, T008 | Slurm/resource manifest contains no H100 |
| FR-006 | generic secured collaboration architecture | T003, T005 | assignment, tokens, operation-status and dependency receipts |
| FR-007 | placement/runtime contract | T002, T004, T005 | CUDA load/forward, peak memory, `cpuFallback=0` |
| FR-008, FR-009 | generation evidence contract | T005, T006 | ordered exact tokens, decoded text, EOS within 64 |
| FR-010 | generation evidence contract | T006, T007 | five warmups and 25 measured rows |
| FR-011, FR-012 | `data-model.md`; generation evidence contract | T006, T007 | raw JSONL and reproducible per-prompt/pooled summaries |
| FR-013 | generation evidence summary rules | T006, T007 | retained exclusion counts and filtered successful summaries |
| FR-014 | `quickstart.md` and plan safety | T003-T007 | bounded Slurm identities; no login-node compute |
| FR-015 | plan safety/rollback | T005, T007 | exactly-once ledger and first terminal result |
| FR-016 | separate Spec 161 identity | T001, T008 | unchanged Job 174382 record and separate Spec 162 IDs |
| FR-017 | architecture pause boundary | Spec 163, T009 | pause ledger, no live mutation, fresh requalification and authorization |
| FR-018 | `quickstart.md`; request-driven generation evidence contract | T009, T005 | fixed-settle source guard, `SPEC162_REQUEST_GATE_OPEN`, ACK-closed planning, and dependency-driven stage markers |

| Success criterion | Closing task | Required evidence |
|---|---|---|
| SC-001 | T005, T007 | 25 accepted exact EOS generations |
| SC-002 | T005, T007 | per-token three-stage/two-dependency correlation |
| SC-003 | T006, T007 | five warmups plus 25 measured raw rows |
| SC-004 | T006, T007 | raw timing fields reproduce every reported metric |
| SC-005 | T005, T008 | three RTX 5000 nodes/UUIDs, zero fallback |
| SC-006 | T003, T004, T008 | no H100 in release or allocation manifests |
| SC-007 | T007, T008 | bounded final claims and preserved negative evidence |

## Current evidence level

All live FR/SC closure that depends on T005–T008 is currently
`BLOCKED_DEPENDENCY`, not failed or complete. The controlling dependency is
`specs/163-di-collaboration-planning`, followed by T009 fresh
runtime/evidence requalification and a new explicit live authorization.
Historical quota, capacity, runtime, and Job 175053 evidence remains retained.

- Model identity and official architecture: `measured` from official repository
  metadata retrieved 2026-07-28.
- iTiger RTX inventory and partition limits: `measured` by read-only Slurm
  discovery on 2026-07-28.
- Three-stage 32 GB fit: `estimated`; T004 installed the strict load/forward
  and 1 GiB-headroom gate, but T005 must produce the first real measurements.
- Qwen3.6 stage compatibility: `implemented/local-contract-tested`; the
  additive loader covers nested text config, strict stage-local key mapping,
  four-plane positions, hybrid masks, and non-thinking formatting. A real
  Transformers 5.14.1 public Qwen3.5 imports and container CUDA visibility
  passed T003. Full checkpoint load and stage forward remain blocked on the
  Spec 163 architecture dependency, T009 requalification, and a new live
  authorization; the earlier quota decision remains historical evidence.
- Complete-generation and campaign behavior: generation state machine is
  `implemented` under Spec 161 fixtures; Qwen3.6 integration is unexecuted.
- Spec 162 live evidence: T003 Jobs 174578/174594 and T004 no-download Job
  174610 passed. T009 small-Qwen Job 181876 is immutable `FAILED 1:0` after
  request-first ACK/Repo/Selection and multi-stage CUDA execution because the
  frozen reference disagreed with the current runtime at token 8. No
  complete-generation PASS is claimed; the next identity must use the
  current-runtime reference and the request-gate guard.
