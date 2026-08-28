#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=32G
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=00:30:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec160/qwen-stages/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec160/qwen-stages/slurm-%j.err

set -Eeuo pipefail
umask 077
: "${SPEC160_STAGE_RUN_ID:?}"
: "${SPEC160_STAGE_SUBMISSION_ID:?}"
: "${SPEC160_STAGE_SOURCE_ROOT:?}"
: "${SPEC160_STAGE_SOURCE_SHA256:?}"
: "${SPEC160_SIF:?}"
: "${SPEC160_SIF_SHA256:?}"
: "${SPEC160_QWEN_MODEL_DIR:?}"
: "${SPEC160_QWEN_MODEL_REVISION:?}"

readonly evidence_parent='/project/tma1/ndnsf-di/evidence/spec160/qwen-stages'
readonly artifact_parent='/project/tma1/ndnsf-di/artifacts/spec160/qwen-stages'
readonly partial="${evidence_parent}/.${SPEC160_STAGE_SUBMISSION_ID}.partial"
readonly final="${evidence_parent}/${SPEC160_STAGE_SUBMISSION_ID}"
readonly artifact_dir="${artifact_parent}/${SPEC160_STAGE_SUBMISSION_ID}"
test ! -e "$partial" && test ! -e "$final" && test ! -e "$artifact_dir"
mkdir -p "$partial/source" "$artifact_dir"
cp -a "$SPEC160_STAGE_SOURCE_ROOT"/llm_pipeline "$partial/source/"
cp -a "$SPEC160_STAGE_SOURCE_ROOT"/ndnsf_distributed_inference "$partial/source/"
cp "$SPEC160_STAGE_SOURCE_ROOT"/ndnsf.py "$partial/source/"
cp "$SPEC160_STAGE_SOURCE_ROOT"/prepare-qwen-stages.sbatch "$partial/source/"
printf '%s  source-bundle\n' "$SPEC160_STAGE_SOURCE_SHA256" > "$partial/source-bundle.sha256"
printf '%s  %s\n' "$SPEC160_SIF_SHA256" "$SPEC160_SIF" | sha256sum -c - \
  > "$partial/sif-sha256.log"
hostname > "$partial/hostname.txt"
nvidia-smi --query-gpu=uuid,name,driver_version,memory.total \
  --format=csv,noheader > "$partial/gpu.csv"
test "$(wc -l < "$partial/gpu.csv")" -eq 1

terminal_state=FAIL
finish()
{
  rc=$?
  trap - EXIT INT TERM
  python3 - "$partial/result.json" "$terminal_state" "$rc" <<'PY' || true
import json,os,sys
path,state,rc=sys.argv[1:]
with open(path,"w",encoding="utf-8") as f:
  json.dump({"schemaVersion":"spec160-qwen-stage-prep-v1","state":state,
             "exitCode":int(rc),"jobId":os.environ["SLURM_JOB_ID"],
             "runId":os.environ["SPEC160_STAGE_RUN_ID"],
             "submissionId":os.environ["SPEC160_STAGE_SUBMISSION_ID"]},
            f,indent=2,sort_keys=True)
  f.write("\n")
PY
  find "$partial" -type f -print0 | sort -z | xargs -0 sha256sum \
    > "$partial/checksums.sha256" 2>/dev/null || true
  if test "$terminal_state" = PASS && test "$rc" -eq 0; then
    mv "$partial" "$final"
  fi
  exit "$rc"
}
trap finish EXIT INT TERM

comment=$(scontrol show job "$SLURM_JOB_ID" -o | tr ' ' '\n' | sed -n 's/^Comment=//p')
test "$comment" = "spec160:${SPEC160_STAGE_SUBMISSION_ID}"

apptainer exec --nv --cleanenv --containall \
  --home "$partial/home:/home/${USER}" \
  --bind "$partial:$partial:rw" \
  --bind "$artifact_dir:$artifact_dir:rw" \
  --bind "$SPEC160_QWEN_MODEL_DIR:$SPEC160_QWEN_MODEL_DIR:ro" \
  --bind "$partial/source:/source:ro" \
  --env "PYTHONPATH=/source:/source/llm_pipeline" \
  --env "HF_HOME=$partial/hf-home" \
  --env "TRANSFORMERS_OFFLINE=1" \
  --env "HF_HUB_OFFLINE=1" \
  "$SPEC160_SIF" \
  python3 /source/llm_pipeline/plan_pipeline.py \
    --policy "$artifact_dir/policy.yaml" \
    --runtime qwen-transformers \
    --stages 3 \
    --layers 24 \
    --qwen-model "$SPEC160_QWEN_MODEL_DIR" \
    --qwen-revision "$SPEC160_QWEN_MODEL_REVISION" \
    --qwen-prompt "NDN distributed inference test" \
    --qwen-dtype float16 \
    > "$partial/plan-pipeline.log" 2>&1

apptainer exec --nv --cleanenv --containall \
  --home "$partial/home:/home/${USER}" \
  --bind "$partial:$partial:rw" \
  --bind "$artifact_dir:$artifact_dir:rw" \
  --bind "$SPEC160_QWEN_MODEL_DIR:$SPEC160_QWEN_MODEL_DIR:ro" \
  --bind "$partial/source:/source:ro" \
  --env "PYTHONPATH=/source:/source/llm_pipeline" \
  --env "HF_HOME=$partial/hf-home" \
  --env "TRANSFORMERS_OFFLINE=1" \
  --env "HF_HUB_OFFLINE=1" \
  "$SPEC160_SIF" \
  python3 - "$artifact_dir" "$partial/stage-manifest.json" \
  > "$partial/cuda-load.log" 2>&1 <<'PY'
import hashlib
import json
import sys
from pathlib import Path

import torch

from llm_pipeline_lib import qwen_transformer_model_from_stage_package

artifact_dir = Path(sys.argv[1])
manifest_path = Path(sys.argv[2])
stage_paths = sorted((artifact_dir / "qwen-transformers-stage-artifacts").glob("stage-*-qwen-transformers.pt"))
if len(stage_paths) != 3:
    raise SystemExit(f"expected 3 Qwen stage packages, found {len(stage_paths)}")
stages = []
for path in stage_paths:
    package = torch.load(path, map_location="cpu", weights_only=True)
    spec = dict(package["spec"])
    model = qwen_transformer_model_from_stage_package(path, device="cuda:0", require_cuda=True)
    first_param = next(model.parameters())
    if first_param.device.type != "cuda":
        raise SystemExit(f"stage {path.name} did not load on CUDA")
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    stages.append({
        "filename": path.name,
        "path": str(path),
        "sha256": digest,
        "bytes": path.stat().st_size,
        "role": spec["role"],
        "stageIndex": int(spec["stageIndex"]),
        "stageCount": int(spec["stageCount"]),
        "layerCount": int(spec["layerCount"]),
        "layerRange": dict(spec["layerRange"]),
        "runtime": spec["runtime"],
        "cudaDevice": str(first_param.device),
    })
    del model
    torch.cuda.empty_cache()
policy = artifact_dir / "policy.yaml"
runtime = artifact_dir / "qwen-pipeline-runtime.json"
doc = {
    "schemaVersion": "spec160-qwen-stage-manifest-v1",
    "artifactDir": str(artifact_dir),
    "policy": {
        "path": str(policy),
        "sha256": hashlib.sha256(policy.read_bytes()).hexdigest(),
        "bytes": policy.stat().st_size,
    },
    "runtimeSummary": {
        "path": str(runtime),
        "sha256": hashlib.sha256(runtime.read_bytes()).hexdigest(),
        "bytes": runtime.stat().st_size,
    },
    "stages": stages,
}
manifest_path.write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n", encoding="utf-8")
print(json.dumps(doc, indent=2, sort_keys=True))
PY

cp "$partial/stage-manifest.json" "$artifact_dir/stage-manifest.json"
touch "$artifact_dir/ACCEPTED"
terminal_state=PASS
