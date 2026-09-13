# Spec186 Validation Matrix

| ID | Gate / task | Positive or negative case | Required evidence | Status at planning |
| --- | --- | --- | --- | --- |
| V01 | T001 | Exact `575b43c` source and clean branch | `baseline-inventory.md`, parent/tree/SHA, clean status | NOT_RUN |
| V02 | T002/T003 | Profile, path, hash, mode, schema and candidate mutations | rejection code, restart gate, zero side effects | NOT_RUN |
| V03 | T004 | MiniNDN/Tiger lifecycle with explicit sockets, routes and cleanup | argv/env, process map, readiness barrier, cleanup | NOT_RUN |
| V04 | T005 | Production path convergence | CodeGraph traceability and `PASS` audit | NOT_RUN |
| V05 | T006 | Build/runtime/ABI and layered base+app composition | builder receipt, import/help, `readelf`, `ldd -r`, SIF/app hashes | NOT_RUN |
| V06 | T007 | YOLO MiniNDN Y-A/Y-B normal | terminal response, `[1,50,6]`, oracle, dependencies, cleanup | NOT_RUN |
| V07 | T007 | YOLO MiniNDN Y-N registration/dependency negatives | expected rejection, unique edge, no response/reselection, cleanup | NOT_RUN |
| V08 | T008 | Qwen3-0.6B CPU cold request and follow-up | model/backend/tokenizer digest, token IDs, checkpoint, terminal, cleanup | NOT_RUN |
| V09 | T009 | Tiger single-node YOLO GPU | allocation, CUDA model roles, CPU Merge, oracle, exit, cleanup | NOT_RUN |
| V10 | T010 | Tiger two-node YOLO normal | two hosts, four roles, cross-node Data, 1 warmup + 3 measured, oracle | NOT_RUN |
| V11 | T010 | Tiger two-node dependency negative | exact producer/consumer edge, bounded failure, no success/reselection, cleanup | NOT_RUN |
| V12 | T011 | Conditional Tiger Qwen3-0.6B | only if artifacts/backend/resources complete; otherwise bounded waiting receipt | NOT_RUN |
| V13 | T012 | Independent two-node normal reuse | same candidate/profile/artifact hashes, new allocation/identities, 1+3 requests | NOT_RUN |
| V14 | T013 | Offline handoff/reanalysis | reproducible commands, hashes, raw index and failure-log links | NOT_RUN |

## Evidence Rules

`IMPLEMENTED` means code or documentation exists. `EXECUTED` means a real path ran.
`VERIFIED` is limited to the declared gate. `PASS` requires protocol/result/exit/cleanup
agreement for one candidate. A fixture, component check, old candidate, READY marker or
partial failure remains lower-scope evidence.

Local raw results belong under `Experiments/TigerCluster/results/<run-id>` or declared
project storage. Spec evidence stores concise receipts and hashes. A failed run is never
rewritten; a repair receives a new candidate or run identity according to the invalidation
matrix.
