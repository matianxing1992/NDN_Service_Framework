# Spec186 static-review experiment cycle — 2026-09-13

This cycle follows the required order: static review, executable boundary
experiment, defect repair, and a second static/test review. It exercises the
actual `submit.py check` entrypoint for every declared profile and the actual
`run.sbatch` shell payload with a deliberately mismatched identity environment.
No SSH, rsync, staging, scheduler submission or model process is started.

## Pre-dispatch matrix

Each candidate manifest was rebuilt from its profile, then checked with:

```text
python3 Experiments/TigerCluster/jobs/spec184/submit.py check \
  --profile Experiments/TigerCluster/profiles/<profile>.json \
  --candidate <fresh-manifest.json>
```

| Profile | CLI result | Failure count | SSH/rsync/staging/sbatch |
| --- | --- | ---: | --- |
| `spec184-qwen06b-minindn-cpu` | `ok=false` | 13 | `0/0/0/0` |
| `spec184-qwen06b-tiger-experimental` | `ok=false` | 19 | `0/0/0/0` |
| `spec184-yolo-minindn-negative` | `ok=false` | 4 | `0/0/0/0` |
| `spec184-yolo-minindn-normal` | `ok=false` | 4 | `0/0/0/0` |
| `spec184-yolo-tiger-single-gpu` | `ok=false` | 13 | `0/0/0/0` |
| `spec184-yolo-tiger-two-node-negative` | `ok=false` | 13 | `0/0/0/0` |
| `spec184-yolo-tiger-two-node-normal` | `ok=false` | 13 | `0/0/0/0` |
| `spec184-yolo-tiger-two-node-reuse` | `ok=false` | 13 | `0/0/0/0` |

The first boundaries remain the declared missing base SIF/application/model
inputs, GPU placeholders, and the known YOLO/Qwen harness contract drift. The
fail-closed result is expected and is not a runtime qualification failure.

## Defect found and repair

The scheduler payload accepted an allow-listed `SPEC186_CANDIDATE_DIGEST` value
that differed from the candidate digest in `SPEC186_EFFECTIVE_CONFIG`. A direct
payload experiment reproduced the defect before repair: the child could have
reported a different provenance identity. The repair added value-level checks
for `SPEC186_RUN_ID` and `SPEC186_CANDIDATE_DIGEST` before run-root creation.

The repaired experiment used a mismatched digest and produced:

```text
rc=1 root_exists=False stderr=SPEC186_EXEC_ENV_IDENTITY_MISMATCH
```

## Re-review

The focused regression suite passes 44 tests and the complete TigerCluster
suite passes 106 tests. The new mutation test confirms the mismatch is rejected
with no run-root side effect. `pyflakes`, `compileall`, `bash -n`, `shellcheck`,
JSON parsing, CodeGraph sync and Spec Kit structural audit also pass. Exact
source-sealed SIF composition, YOLO MiniNDN execution, CUDA/Tiger jobs, Qwen3
artifacts and numerical oracle receipts remain external gates.
