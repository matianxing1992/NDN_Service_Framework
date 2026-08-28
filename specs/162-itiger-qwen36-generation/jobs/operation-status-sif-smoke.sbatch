#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=5G
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=00:10:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec162/operation-status-sif-smoke/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec162/operation-status-sif-smoke/slurm-%j.err

set -Eeuo pipefail
umask 077

readonly SIF='/project/tma1/ndnsf-di/releases/spec162-6216ef2b9525/runtime.sif'
readonly SIF_SHA256="${SPEC162_SIF_SHA256:?SPEC162_SIF_SHA256_REQUIRED}"
readonly SOURCE_ROOT='/project/tma1/ndnsf-di/src/spec162'
readonly SUBMISSION_ID='spec162-submission-operation-status-sif-6216ef2b9525-001'
readonly RUN_ID='spec162-run-operation-status-sif-6216ef2b9525-001'
readonly EVIDENCE_PARENT='/project/tma1/ndnsf-di/evidence/spec162/operation-status-sif-smoke'

: "${SLURM_JOB_ID:?SLURM_JOB_ID_REQUIRED}"
comment=$(scontrol show job "$SLURM_JOB_ID" -o | tr ' ' '\n' | sed -n 's/^Comment=//p')
test "$comment" = "spec162:${SUBMISSION_ID}" || {
  echo SUBMISSION_COMMENT_INVALID >&2
  exit 4
}

partial="${EVIDENCE_PARENT}/.${SUBMISSION_ID}.partial"
final="${EVIDENCE_PARENT}/${SUBMISSION_ID}"
test ! -e "$partial" && test ! -e "$final"
mkdir -p "$partial"

base="${SLURM_TMPDIR:-/tmp/$USER}"
case "$(realpath -m "$base")" in
  /home|/home/*|/project|/project/*) base="/tmp/$USER" ;;
esac
mkdir -p "$base"
scratch=$(mktemp -d "$base/spec162-operation-status-${SLURM_JOB_ID}.XXXXXX")
mkdir -p "$scratch/home"

terminal_state='FAIL'
finish()
{
  rc=$?
  trap - EXIT INT TERM
  if test -d "$scratch"; then
    rm -f -- "$scratch/bootstrap-tokens.txt"
    rm -rf -- "$scratch/home/.ndn" "$scratch/app-state"
    cp -a "$scratch/." "$partial/" 2>/dev/null || true
    rm -rf -- "$scratch"
  fi
  python3 - "$partial/result.json" "$terminal_state" "$rc" "$SLURM_JOB_ID" <<'PY' || true
import json, sys
path, state, rc, job = sys.argv[1:]
with open(path, "w", encoding="utf-8") as stream:
    json.dump({
        "schemaVersion": "spec162-slurm-terminal-v1",
        "state": state,
        "exitCode": int(rc),
        "jobId": job,
        "submissionId": "spec162-submission-operation-status-sif-6216ef2b9525-001",
        "runId": "spec162-run-operation-status-sif-6216ef2b9525-001",
    }, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY
  if test "$terminal_state" = PASS && test "$rc" -eq 0; then
    (
      cd "$partial"
      find . -type f ! -name checksums.sha256 -print0 |
        sort -z |
        xargs -0 sha256sum
    ) > "$partial/checksums.sha256"
    mv "$partial" "$final"
  fi
  exit "$rc"
}
trap finish EXIT INT TERM

printf '%s  %s\n' "$SIF_SHA256" "$SIF" |
  sha256sum -c - > "$partial/sif-sha256.log"
{
  echo "host=$(hostname)"
  echo "jobId=$SLURM_JOB_ID"
  echo "submissionId=$SUBMISSION_ID"
  echo "runId=$RUN_ID"
  echo "sif=$SIF"
  echo "sifSha256=sha256:$SIF_SHA256"
  echo "gres=gpu:rtx_5000:1"
  echo "slurmMemory=5G"
  echo "runtime=fake"
  echo "stages=1"
  echo "modelLoad=disabled"
  nvidia-smi --query-gpu=index,name,uuid,memory.total \
    --format=csv,noheader
} > "$partial/environment.txt"

apptainer exec --nv --cleanenv --containall \
  --home "$scratch/home:/home/tma1" \
  "$SIF" \
  /usr/local/bin/ndnsf-di-verify-qwen36-runtime \
  --lock /opt/ndnsf-di/locks/qwen36-overlay.lock.json \
  > "$partial/qwen36-runtime-gpu-probe.json"
grep -Fq '"status": "PASS"' "$partial/qwen36-runtime-gpu-probe.json"
grep -Fq '"cudaAvailable": true' "$partial/qwen36-runtime-gpu-probe.json"
grep -Fq 'RTX 5000' "$partial/environment.txt"

apptainer exec --nv --cleanenv --containall \
  --home "$scratch/home:/home/tma1" \
  --bind "$scratch:/scratch:rw" \
  --bind "$SOURCE_ROOT:/harness:ro" \
  --bind "$SOURCE_ROOT/llm_pipeline:/source/llm_pipeline:ro" \
  --env PYTHONNOUSERSITE=1,PYTHONDONTWRITEBYTECODE=1 \
  "$SIF" /harness/local-docker-operation-status-inner.sh \
  > "$scratch/smoke.log" 2>&1

grep -Fq SPEC160_LOCAL_DOCKER_OPERATION_STATUS_SMOKE_PASS "$scratch/smoke.log"
grep -Fq LLM_PIPELINE_STAGE_FINAL "$scratch/provider.log"
awk -F, 'NR > 1 && $1 == "measured" && $4 == "ok" { found = 1 } END { exit !found }' \
  "$scratch/user-metrics.csv"
if grep -Fq "is not an instance of 'none'" "$scratch/provider.log"; then
  echo OPERATION_STATUS_BINDING_REGRESSION >&2
  exit 8
fi
test -z "$(find "$scratch" -type f \
  \( -name '*.privkey' -o -name 'bootstrap-tokens.txt' \) -print -quit)"

terminal_state='PASS'
