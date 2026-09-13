# Spec186 evidence index

These receipts are the durable record for the current candidate boundary. A
document with `WAITING_EXTERNAL_INPUT` or `BLOCKED_AFTER_BOUNDARY` is evidence
of the stopping point; it is not a qualification result.

| Artifact | Scope | Current boundary |
| --- | --- | --- |
| `baseline-inventory.md` | exact source, host tools, model inventory | Tiger/GPU/Qwen3 inputs absent locally |
| `portability-audit.md` | caller paths, ownership and cross-host assumptions | external staging and route receipt pending |
| `design-code-convergence.md` | implementation-to-contract audit | `BLOCK` until native/runtime closure |
| `native-abi-closure.md` | Waf, ONNX/NAC-ABE, import/help, RPATH/`ldd` | pinned Rust tokenizer input absent |
| `pre-dispatch-boundary-20260912.md` | all eight candidate prepare/check receipts | zero remote side effects; no runtime started |
| `minindn-yolo-boundary-20260912.md` | fresh Y-A/Y-B/Y-N attempts | environment preflight stopped before startup |
| `minindn-qwen06b-inventory.md` | Qwen3-0.6B model/backend inventory | compatible model tuple absent |
| `tiger-preflight-20260912.md` | Tiger Slurm/GPU/project/Apptainer preflight | compute GPU available; SIF/runtime composition still pending |
| `closure-handoff-20260912.md` | implemented/wired/executed/measured reconciliation | branch remains `IN_PROGRESS` with ordered recovery gates |

Runtime receipts belong in `Experiments/TigerCluster/results/<run-id>` or the
declared project-storage counterpart. Secrets, models, SIFs and large logs do
not belong in this index or in Git.
