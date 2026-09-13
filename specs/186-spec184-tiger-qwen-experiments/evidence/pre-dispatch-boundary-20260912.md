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

## Refresh after native rebuild — 2026-09-12

After rebuilding the provider and extension, updating all profile application
hashes and fixing the canonical extension import probe, every profile was
prepared and checked again. The new digests are the only identities valid for
subsequent evidence:

| Profile | Refreshed candidate digest | Check boundary |
| --- | --- | --- |
| `spec184-yolo-minindn-normal` | `8f9eef2710ef98341f1bd0e91aab6579ff61b6ed5742b3bd57fa45da743f2539` | base SIF missing; provider `--help` status 2 |
| `spec184-yolo-minindn-negative` | `0569c17b744252787a5b9cee58a25c9da9366898c5b6ec38e0a555903b522775` | base SIF missing; provider `--help` status 2 |
| `spec184-qwen06b-minindn-cpu` | `64f26d7a84ae98ab5a3b8f898309105099d0a803a15a176eb6b4321c4d570822` | base SIF/model/tokenizer/stage manifest missing; provider `--help` status 2 |
| `spec184-yolo-tiger-single-gpu` | `79044c5b355b50f020a2655e9398974cdb71b306c8c1738790e616d007dab57c` | base SIF and staged model missing; provider `--help` status 2 |
| `spec184-yolo-tiger-two-node-normal` | `512270d0392af31bfbe435566a12d0039e77e6024d649c2f2c82f88a9ea2f780` | base SIF and staged model missing; provider `--help` status 2 |
| `spec184-yolo-tiger-two-node-negative` | `cdeea8db61728c862b8bd4eee40e7b475b10bdac1cbc2f461ae09198ce0a7dcc` | base SIF and staged model missing; provider `--help` status 2 |
| `spec184-yolo-tiger-two-node-reuse` | `9218f38791139daf58cca308f17f59de035a133f2e68bf9f6575967058d4b77c` | base SIF and staged model missing; provider `--help` status 2 |
| `spec184-qwen06b-tiger-experimental` | `ef0af10899e6aa803dc9b8275a953a9e0f7c010f9a2be8725a0cfec045e577cc` | base SIF/model/tokenizer/stage manifest missing; provider `--help` status 2 |

All eight refreshed checks report `ssh=0`, `rsync=0`, `staging=0` and
`sbatch=0`. The old table above remains as historical evidence; its digests
must not be mixed with this refreshed candidate set.
