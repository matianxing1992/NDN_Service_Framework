#!/bin/bash
set -Eeuo pipefail
umask 077

: "${SLURM_JOB_ID:?SLURM_JOB_ID_REQUIRED}"
: "${SPEC159_RUN_ID:?SPEC159_RUN_ID_REQUIRED}"
: "${SPEC159_SUBMISSION_ID:?SPEC159_SUBMISSION_ID_REQUIRED}"
: "${SPEC159_REPLACES_JOB:?SPEC159_REPLACES_JOB_REQUIRED}"

readonly PROJECT_ROOT='/project/tma1/ndnsf-di'
readonly SIF="${SPEC159_SIF:-${PROJECT_ROOT}/releases/spec159-a0e58822fe127594/runtime.sif}"
readonly SIF_SHA256="${SPEC159_SIF_SHA256:-fee3c83f2520e63359a7e6cba82c43ea8d040d8ac5573dfcd19732f00e1eda1a}"
readonly REQUEST_ID="${SPEC159_REQUEST_ID:-spec159-qwen-live-001}"
readonly MODEL_REVISION='7ae557604adf67be50417f59c2c2f167def9a775'
readonly MODEL="${PROJECT_ROOT}/models/source/qwen25-0.5b/${MODEL_REVISION}"
readonly MODEL_MANIFEST="${PROJECT_ROOT}/src/spec159/qwen-model-manifest.sha256"
readonly SOURCE_ROOT="${PROJECT_ROOT}/src/spec159"
readonly EVIDENCE_PARENT="${PROJECT_ROOT}/evidence/spec159/ndnsf-di"

comment=$(scontrol show job "$SLURM_JOB_ID" -o | tr ' ' '\n' | sed -n 's/^Comment=//p')
test "$comment" = "spec159:${SPEC159_SUBMISSION_ID}" || {
  echo SUBMISSION_COMMENT_INVALID >&2; exit 4;
}

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
  if test -n "$scratch" && test -d "$scratch/log"; then
    cp -a "$scratch/log/." "$partial/" 2>/dev/null || true
  fi
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
  if test -n "$scratch" && test -d "$scratch"; then rm -rf "$scratch"; fi
  if test "$terminal_state" = PASS && test "$rc" -eq 0; then mv "$partial" "$final"; fi
  exit "$rc"
}
trap finish EXIT INT TERM

for path in "$SIF" "$MODEL_MANIFEST" \
  "$SOURCE_ROOT/ndnsf-qwen-app.py" "$SOURCE_ROOT/ndnsf-qwen-inner.sh" \
  "$SOURCE_ROOT/spec159-trust-schema.conf" "$SOURCE_ROOT/spec159-nfd.conf.in"; do
  test -f "$path" || { echo "SEALED_INPUT_MISSING:$path" >&2; exit 4; }
done

base="${SLURM_TMPDIR:-/tmp/$USER}"
case "$(realpath -m "$base")" in /home|/home/*|/project|/project/*) base="/tmp/$USER" ;; esac
mkdir -p "$base"
scratch=$(mktemp -d "$base/spec159-ndnsf-qwen-${SLURM_JOB_ID}.XXXXXX")
mkdir -p "$scratch/home" "$scratch/log" "$scratch/run"

cp "$SOURCE_ROOT/ndnsf-qwen-app.py" "$scratch/"
cp "$SOURCE_ROOT/ndnsf-qwen-inner.sh" "$scratch/"
cp "$SOURCE_ROOT/spec159-trust-schema.conf" "$scratch/trust-schema.conf"
sed -e 's|@@NFD_SOCKET@@|/scratch/run/nfd.sock|g' \
    -e 's|@@TCP_PORT@@|6363|g' \
    -e 's|@@UDP_PORT@@|6363|g' \
    -e 's|@@NODE_RANK@@|0|g' \
    -e 's|@@STATE_DIR@@|/scratch|g' \
    -e '/^[[:space:]]*status[[:space:]]*$/d' \
    "$SOURCE_ROOT/spec159-nfd.conf.in" > "$scratch/nfd.conf"
cat > "$scratch/qwen.policies" <<'POLICY'
name /example/hello/controller/NDNSF/ControllerPolicy/v1

provider-policies
{
  provider-policy
  {
    for /example/hello/provider
    allow
    {
      /AI/LLM/Qwen
    }
  }
}

user-policies
{
  user-policy
  {
    for /example/hello/user
    allow
    {
      /AI/LLM/Qwen
    }
  }
}
POLICY

cp "$SOURCE_ROOT/ndnsf-qwen-app.py" "$SOURCE_ROOT/ndnsf-qwen-inner.sh" \
  "$SOURCE_ROOT/spec159-trust-schema.conf" "$SOURCE_ROOT/spec159-nfd.conf.in" \
  "$partial/"
cp "$scratch/qwen.policies" "$partial/"
sha256sum "$partial/ndnsf-qwen-app.py" "$partial/ndnsf-qwen-inner.sh" \
  "$partial/spec159-trust-schema.conf" "$partial/spec159-nfd.conf.in" \
  "$partial/qwen.policies" > "$partial/input-sha256.txt"
printf '%s  %s\n' "$SIF_SHA256" "$SIF" | sha256sum -c - > "$partial/sif-sha256.log"
(cd "$MODEL" && sha256sum -c "$MODEL_MANIFEST") > "$partial/model-sha256.log"
nvidia-smi --query-gpu=uuid,name,driver_version,memory.total \
  --format=csv,noheader > "$partial/host-gpu.csv"
test "$(wc -l < "$partial/host-gpu.csv")" -eq 1 || {
  echo GPU_ALLOCATION_NOT_EXACTLY_ONE >&2; exit 4;
}

apptainer exec --nv --cleanenv --containall \
  --home "$scratch/home:/home/tma1" \
  --bind "$scratch:/scratch:rw" \
  --bind "$MODEL:/models/qwen:ro" \
  --env "SLURM_JOB_ID=${SLURM_JOB_ID},SPEC159_REQUEST_ID=${REQUEST_ID},PYTHONNOUSERSITE=1,PYTHONDONTWRITEBYTECODE=1,HF_HUB_OFFLINE=1,TRANSFORMERS_OFFLINE=1" \
  "$SIF" /scratch/ndnsf-qwen-inner.sh \
  > "$partial/orchestration.log" 2>&1

cp "$scratch/ndnsf-result.json" "$partial/ndnsf-result.json"
cp -a "$scratch/log/." "$partial/"
python3 - "$partial/ndnsf-result.json" "$partial/host-gpu.csv" "$REQUEST_ID" <<'PY'
import csv,json,sys
result=json.load(open(sys.argv[1],encoding='utf-8'))
gpu=next(csv.reader(open(sys.argv[2],encoding='utf-8')))[0].strip()
request_id=sys.argv[3]
assert result["status"] == "PASS"
assert result["service"] == "/AI/LLM/Qwen"
assert result["requestId"] == request_id
assert result["revision"] == "7ae557604adf67be50417f59c2c2f167def9a775"
assert result["modelWeightSha256"] == "sha256:fdf756fa7fcbe7404d5c60e26bff1a0c8b8aa1f72ced49e7dd0210fe288fb7fe"
assert result["backend"] == "transformers-cuda"
assert result["cpuFallback"] is False
assert result["gpuUuid"] == gpu
assert result["generatedTokens"] > 0 and result["generatedText"].strip()
PY
find "$partial" -maxdepth 1 -type f -print0 | sort -z | xargs -0 sha256sum \
  > "$partial/checksums.sha256"
terminal_state='PASS'
