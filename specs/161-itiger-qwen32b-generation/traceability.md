# Traceability: iTiger Qwen 32B Multi-Generation Campaign

Status labels:

- `planned`: contract/task exists; behavior is not yet implemented.
- `implemented`: source and local tests pass.
- `executed`: a real path ran without full acceptance evidence.
- `measured`: the formal live evidence satisfies the criterion.

Current feature state: T001/T002/T003/T005 are complete with local tests.
Generation, campaign, analyzer, and fail-closed capacity logic are
`implemented`. The bounded scratch probe and capacity decision are `measured`;
no model preparation, CUDA inference, or formal-campaign claim is `executed` or
`measured`.

## Requirements to tasks

| Requirement | Owning task(s) | Planned acceptance |
|---|---|---|
| FR-001, FR-004 | T001, T004 | Frozen revision, tokenizer/chat template, prompt/reference digests |
| FR-002, FR-003 | T004, T006 | Three distinct RTX 5000 nodes/UUIDs, exact layer coverage, zero CPU fallback |
| FR-005, FR-006, FR-007 | T003, T004 | `implemented` locally: context-growing greedy loop, EOS/text result, full-sequence fixture match; live reference remains planned |
| FR-008, FR-009 | T005, T006 | `implemented` locally: five prompts, one warmup plus five measured, stable generation/token IDs; live campaign remains planned |
| FR-010, FR-011, FR-012 | T005, T006 | `implemented` locally: raw JSONL and reproducible per-prompt/descriptive pooled summaries; measured distributions remain planned |
| FR-013, FR-014 | T004, T006 | Secured request and correlated three-stage/two-dependency receipts per token |
| FR-015 | T004 | H100 same-revision/dtype/template reference |
| FR-016, FR-017, FR-018 | T002, T004 | `measured` capacity pass from Slurm job 174363 plus the published 1 TB project policy; exact T004 promotion bytes remain gated |
| FR-019 | T002, T007 | `implemented` explicit cleanup inventory with no implicit deletion; final audit remains planned |
| FR-020, FR-021 | T004, T006 | Bounded Slurm and exactly-once identities |
| FR-022 | T004, T006, T007 | Credential scan and evidence exclusion |
| FR-023 | T007 | Capability-bounded final report |

## Success criteria to evidence

| Criterion | Owning task(s) | Evidence class before execution |
|---|---|---|
| SC-001 | T003, T004 | implemented locally; live acceptance planned |
| SC-002 | T005, T006 | implemented locally; formal 25-sample acceptance planned |
| SC-003 | T004, T006 | planned |
| SC-004 | T005, T006 | analyzer implemented; measured distribution planned |
| SC-005 | T004, T006 | planned |
| SC-006 | T002, T004 | measured capacity decision PASS; exact promoted-byte recheck remains required in T004 |
| SC-007 | T007 | planned |

## User intent chain

```text
large model across three GPUs
  -> US1 -> FR-001..FR-007, FR-013..FR-015
  -> T003/T004 -> SC-001/SC-003/SC-005

real input and complete answer
  -> US1 -> FR-004..FR-007, FR-015
  -> prompt-set contract + T003/T004 -> SC-001

multiple inferences exposing a distribution
  -> US2 -> FR-008..FR-012
  -> evidence contract + T005/T006 -> SC-002/SC-004

use TigerCluster-provided preparation space
  -> US3 -> FR-016..FR-019
  -> T002/T004 -> SC-006
```
