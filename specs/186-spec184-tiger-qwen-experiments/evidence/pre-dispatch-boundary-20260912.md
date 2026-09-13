# Spec186 pre-dispatch receipts

**Date:** 2026-09-12
**Source:** `1e93f0d5` plus the current Spec186 collector
**Command:** `python3 Experiments/TigerCluster/jobs/spec184/submit.py prepare/check`
**Remote side effects:** `ssh=0`, `rsync=0`, `staging=0`, `sbatch=0` for every row

All eight profile manifests were prepared offline. The check command returned a
JSON receipt for each manifest; `ok=false` is expected while the required
external/runtime tuple is absent. Candidate digests are recorded so a later
run cannot silently reuse one of these waiting receipts.

| Profile | Candidate digest (SHA-256) | First reported boundary |
| --- | --- | --- |
| `spec184-yolo-minindn-normal` | `23c5fa845caba2d2fed802a88ade623a8c3205c94c1d40ffb5e22324a875e752` | missing base SIF; native `--help` 127; extension import failure |
| `spec184-yolo-minindn-negative` | `2de4408387f533c18ee36639f163392b3c3c708d8654439a3566f040c9c77469` | missing base SIF; native `--help` 127; extension import failure |
| `spec184-qwen06b-minindn-cpu` | `43ed19cd98a38e2e5c2c36f93a4acc5c968afc6b1cd3d0652da46d1c159aabd9` | Qwen3 model, stage manifest and tokenizer files missing |
| `spec184-yolo-tiger-single-gpu` | `76deaf02f9d322e8583a0b4b9310d20b8832e2e98a2003c68ee0983eb7518ca9` | staged model/base SIF missing; native `--help` 127; extension import failure |
| `spec184-yolo-tiger-two-node-normal` | `ee48a6b0cad02c6d096fb82681a0b62468a3ad3346ccbe1d039168e0a940f71d` | staged model/base SIF missing; native `--help` 127; extension import failure |
| `spec184-yolo-tiger-two-node-negative` | `30f2a9f688628f97c4826697b8d128351182ab0b957903d877a99322a84eb3d4` | staged model/base SIF missing; native `--help` 127; extension import failure |
| `spec184-yolo-tiger-two-node-reuse` | `86423c5801540ac92a30c40ebbac21b912ecf4fda40eda25a3ae8e26d1cc01ef` | staged model/base SIF missing; native `--help` 127; extension import failure |
| `spec184-qwen06b-tiger-experimental` | `25a1cec2d01f9906b19476f242ec05c1463ae55ed6c817fc7e00cf8cefdfe045` | Qwen3 model, stage manifest and tokenizer files missing |

The check receipt also verifies the candidate digest and rejects placeholder
GPU/account inputs before scheduler submission. These are boundary receipts,
not MiniNDN, Slurm, GPU or model execution results.
