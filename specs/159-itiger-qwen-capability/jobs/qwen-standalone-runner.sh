#!/bin/bash
set -Eeuo pipefail
umask 077

: "${SLURM_JOB_ID:?SLURM_JOB_ID_REQUIRED}"
: "${SPEC159_RUN_ID:?SPEC159_RUN_ID_REQUIRED}"
: "${SPEC159_SUBMISSION_ID:?SPEC159_SUBMISSION_ID_REQUIRED}"
: "${SPEC159_REPLACES_JOB:?SPEC159_REPLACES_JOB_REQUIRED}"

readonly PROJECT_ROOT='/project/tma1/ndnsf-di'
readonly SIF="${PROJECT_ROOT}/releases/spec159-a0e58822fe127594/runtime.sif"
readonly SIF_SHA256='fee3c83f2520e63359a7e6cba82c43ea8d040d8ac5573dfcd19732f00e1eda1a'
readonly MODEL_REVISION='7ae557604adf67be50417f59c2c2f167def9a775'
readonly MODEL="${PROJECT_ROOT}/models/source/qwen25-0.5b/${MODEL_REVISION}"
readonly MODEL_MANIFEST="${PROJECT_ROOT}/src/spec159/qwen-model-manifest.sha256"
readonly EVIDENCE_PARENT="${PROJECT_ROOT}/evidence/spec159/standalone"

comment=$(scontrol show job "$SLURM_JOB_ID" -o | tr ' ' '\n' | sed -n 's/^Comment=//p')
test "$comment" = "spec159:${SPEC159_SUBMISSION_ID}" || {
  echo SUBMISSION_COMMENT_INVALID >&2; exit 4;
}
if ! test -f "$SIF" || ! test -d "$MODEL" || ! test -f "$MODEL_MANIFEST"; then
  echo SEALED_INPUT_MISSING >&2
  exit 4
fi

partial="${EVIDENCE_PARENT}/.${SPEC159_SUBMISSION_ID}.partial"
final="${EVIDENCE_PARENT}/${SPEC159_SUBMISSION_ID}"
if test -e "$partial" || test -e "$final"; then
  echo EVIDENCE_IDENTITY_EXISTS >&2
  exit 4
fi
mkdir -p "$partial"

scratch=''
terminal_state='FAIL'
finish()
{
  rc=$?
  trap - EXIT INT TERM
  [ -z "$scratch" ] || [ ! -d "$scratch" ] || rm -rf "$scratch"
  python3 - "$partial/result.json" "$terminal_state" "$rc" "$SLURM_JOB_ID" \
    "$SPEC159_SUBMISSION_ID" "$SPEC159_RUN_ID" "$SPEC159_REPLACES_JOB" <<'PY' || true
import json,sys
path,state,rc,job,submission,run,replaces=sys.argv[1:]
with open(path,'w',encoding='utf-8') as f:
  json.dump({'schemaVersion':'spec159-slurm-terminal-v1','state':state,
             'exitCode':int(rc),'jobId':job,'submissionId':submission,
             'runId':run,'replacesFailedJob':replaces},f,indent=2,sort_keys=True)
  f.write('\n')
PY
  if [ "$terminal_state" = PASS ] && [ "$rc" -eq 0 ]; then mv "$partial" "$final"; fi
  exit "$rc"
}
trap finish EXIT INT TERM

base="${SLURM_TMPDIR:-/tmp/$USER}"
case "$(realpath -m "$base")" in /home|/home/*|/project|/project/*) base="/tmp/$USER" ;; esac
mkdir -p "$base"
scratch=$(mktemp -d "$base/spec159-qwen-${SLURM_JOB_ID}.XXXXXX")
mkdir -p "$scratch/home"

cp "$0" "$partial/qwen-standalone-runner.sh"
cp "$MODEL_MANIFEST" "$partial/qwen-model-manifest.sha256"
sha256sum "$partial/qwen-standalone-runner.sh" "$partial/qwen-model-manifest.sha256" \
  > "$partial/input-sha256.txt"
printf '%s  %s\n' "$SIF_SHA256" "$SIF" | sha256sum -c - > "$partial/sif-sha256.log"
(cd "$MODEL" && sha256sum -c "$MODEL_MANIFEST") > "$partial/model-sha256.log"

{
  echo "host=$(hostname)"
  echo "jobId=$SLURM_JOB_ID"
  echo "submissionId=$SPEC159_SUBMISSION_ID"
  echo "runId=$SPEC159_RUN_ID"
  echo "replacesFailedJob=$SPEC159_REPLACES_JOB"
  echo "sif=$SIF"
  echo "sifSha256=sha256:$SIF_SHA256"
  echo "model=$MODEL"
  echo "modelRevision=$MODEL_REVISION"
  echo "cudaVisibleDevices=${CUDA_VISIBLE_DEVICES:-}"
  id
  apptainer version
} > "$partial/environment.txt"
nvidia-smi --query-gpu=uuid,name,driver_version,memory.total \
  --format=csv,noheader > "$partial/host-gpu.csv"
test "$(wc -l < "$partial/host-gpu.csv")" -eq 1 || {
  echo GPU_ALLOCATION_NOT_EXACTLY_ONE >&2; exit 4;
}

cat > "$scratch/qwen_probe.py" <<'PY'
import json
import os
from pathlib import Path
import time

import torch
from transformers import AutoModelForCausalLM, AutoTokenizer

MODEL = Path("/models/qwen")
REVISION = "7ae557604adf67be50417f59c2c2f167def9a775"
PROMPT = "Reply with one short sentence confirming that Qwen inference is running on TigerCluster."

if not torch.cuda.is_available():
    raise RuntimeError("QWEN_PYTORCH_CUDA_UNAVAILABLE")
if not MODEL.is_dir():
    raise RuntimeError("QWEN_MODEL_BIND_MISSING")

torch.manual_seed(0)
torch.cuda.manual_seed_all(0)
torch.cuda.reset_peak_memory_stats()
started = time.perf_counter()
tokenizer = AutoTokenizer.from_pretrained(
    MODEL, local_files_only=True, trust_remote_code=False
)
model = AutoModelForCausalLM.from_pretrained(
    MODEL,
    local_files_only=True,
    trust_remote_code=False,
    torch_dtype=torch.float16,
    attn_implementation="eager",
)
model.eval()
model.to("cuda:0")
loaded = time.perf_counter()
if model.device.type != "cuda":
    raise RuntimeError("QWEN_MODEL_CPU_FALLBACK")

messages = [
    {"role": "system", "content": "You are a concise validation assistant."},
    {"role": "user", "content": PROMPT},
]
rendered = tokenizer.apply_chat_template(
    messages, tokenize=False, add_generation_prompt=True
)
encoded = tokenizer(rendered, return_tensors="pt")
encoded = {name: value.to("cuda:0") for name, value in encoded.items()}
if any(value.device.type != "cuda" for value in encoded.values()):
    raise RuntimeError("QWEN_INPUT_CPU_FALLBACK")

with torch.inference_mode():
    generated = model.generate(
        **encoded,
        do_sample=False,
        max_new_tokens=24,
        pad_token_id=tokenizer.eos_token_id,
    )
torch.cuda.synchronize()
finished = time.perf_counter()
suffix = generated[0, encoded["input_ids"].shape[1]:]
text = tokenizer.decode(suffix, skip_special_tokens=True).strip()
if suffix.numel() == 0 or not text:
    raise RuntimeError("QWEN_EMPTY_GENERATION")

report = {
    "schemaVersion": "spec159-qwen-standalone-v1",
    "status": "PASS",
    "repository": "Qwen/Qwen2.5-0.5B-Instruct",
    "revision": REVISION,
    "prompt": PROMPT,
    "renderedPrompt": rendered,
    "generatedText": text,
    "inputTokenIds": encoded["input_ids"][0].tolist(),
    "generatedTokenIds": suffix.tolist(),
    "inputTokens": int(encoded["input_ids"].shape[1]),
    "generatedTokens": int(suffix.numel()),
    "loadMs": round((loaded - started) * 1000, 3),
    "generationMs": round((finished - loaded) * 1000, 3),
    "torchVersion": torch.__version__,
    "torchCudaVersion": torch.version.cuda,
    "deviceName": torch.cuda.get_device_name(0),
    "deviceIndex": torch.cuda.current_device(),
    "modelDevice": str(model.device),
    "cpuFallback": False,
    "peakAllocatedBytes": int(torch.cuda.max_memory_allocated()),
    "slurmJobId": os.environ["SLURM_JOB_ID"],
}
Path("/scratch/qwen-result.json").write_text(
    json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8"
)
print(json.dumps(report, indent=2, sort_keys=True))
PY

common=(
  --nv --cleanenv --containall
  --home "$scratch/home:/home/tma1"
  --bind "$scratch:/scratch:rw"
  --bind "$MODEL:/models/qwen:ro"
  --env "SLURM_JOB_ID=${SLURM_JOB_ID},PYTHONNOUSERSITE=1,PYTHONDONTWRITEBYTECODE=1,HF_HUB_OFFLINE=1,TRANSFORMERS_OFFLINE=1"
)
apptainer exec "${common[@]}" "$SIF" \
  /usr/local/bin/ndnsf-di-probe-runtime --mode allocated-gpu \
  --output /scratch/allocated-gpu.json > "$partial/allocated-gpu.log" 2>&1
cp "$scratch/allocated-gpu.json" "$partial/allocated-gpu.json"

apptainer exec "${common[@]}" "$SIF" \
  python3 /scratch/qwen_probe.py > "$partial/qwen.log" 2>&1
cp "$scratch/qwen-result.json" "$partial/qwen-result.json"

python3 - "$partial/allocated-gpu.json" "$partial/qwen-result.json" <<'PY'
import json,sys
gpu=json.load(open(sys.argv[1],encoding='utf-8'))
qwen=json.load(open(sys.argv[2],encoding='utf-8'))
assert gpu["status"] == "PASS"
assert gpu["gpu"]["cpuFallback"] is False
assert qwen["status"] == "PASS"
assert qwen["cpuFallback"] is False
assert qwen["revision"] == "7ae557604adf67be50417f59c2c2f167def9a775"
assert qwen["generatedTokens"] > 0 and qwen["generatedText"].strip()
PY

find "$partial" -maxdepth 1 -type f -print0 | sort -z | xargs -0 sha256sum \
  > "$partial/checksums.sha256"
terminal_state='PASS'
