#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=5G
#SBATCH --time=00:10:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec160/operation-status-sif-smoke/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec160/operation-status-sif-smoke/slurm-%j.err

set -Eeuo pipefail
umask 077

readonly SIF='/project/tma1/ndnsf-di/releases/spec160-af0c3a3ce357-operation-status/runtime.sif'
readonly SIF_SHA256="${SPEC160_SIF_SHA256:?SPEC160_SIF_SHA256_REQUIRED}"
readonly RUN_ID='spec160-run-operation-status-sif-smoke-af0c3a3ce357-003'
readonly SUBMISSION_ID='spec160-submission-operation-status-sif-smoke-af0c3a3ce357-003'
readonly REPLACES_JOB='174170'
readonly PROJECT_ROOT='/project/tma1/ndnsf-di'
readonly SOURCE_ROOT="${PROJECT_ROOT}/src/spec160"
readonly EVIDENCE_PARENT="${PROJECT_ROOT}/evidence/spec160/operation-status-sif-smoke"

: "${SLURM_JOB_ID:?SLURM_JOB_ID_REQUIRED}"
comment=$(scontrol show job "$SLURM_JOB_ID" -o | tr ' ' '\n' | sed -n 's/^Comment=//p')
test "$comment" = "spec160:${SUBMISSION_ID}" || {
  echo SUBMISSION_COMMENT_INVALID >&2
  exit 4
}

partial="${EVIDENCE_PARENT}/.${SUBMISSION_ID}.partial"
final="${EVIDENCE_PARENT}/${SUBMISSION_ID}"
if [ -e "$partial" ] || [ -e "$final" ]; then
  echo EVIDENCE_IDENTITY_EXISTS >&2
  exit 4
fi
mkdir -p "$partial"

base="${SLURM_TMPDIR:-/tmp/$USER}"
case "$(realpath -m "$base")" in
  /home|/home/*|/project|/project/*) base="/tmp/$USER" ;;
esac
mkdir -p "$base"
scratch=$(mktemp -d "$base/spec160-operation-status-${SLURM_JOB_ID}.XXXXXX")
mkdir -p "$scratch/home"

terminal_state='FAIL'
finish()
{
  rc=$?
  trap - EXIT INT TERM
  if [ -n "${scratch:-}" ] && [ -d "$scratch" ]; then
    rm -f -- "$scratch/bootstrap-tokens.txt"
    rm -rf -- "$scratch/home/.ndn" "$scratch/app-state"
    cp -a "$scratch/." "$partial/" 2>/dev/null || true
    rm -rf -- "$scratch"
  fi
  python3 - "$partial/result.json" "$terminal_state" "$rc" "$SLURM_JOB_ID" \
    "$SUBMISSION_ID" "$RUN_ID" "$REPLACES_JOB" <<'PY' || true
import json,sys
path,state,rc,job,submission,run,replaces=sys.argv[1:]
with open(path,'w',encoding='utf-8') as stream:
  json.dump({'schemaVersion':'spec160-slurm-terminal-v1','state':state,
             'exitCode':int(rc),'jobId':job,'submissionId':submission,
             'runId':run,'replacesJob':replaces},
            stream,indent=2,sort_keys=True)
  stream.write('\n')
PY
  if [ "$terminal_state" = PASS ] && [ "$rc" -eq 0 ]; then
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

printf '%s  %s\n' "$SIF_SHA256" "$SIF" | sha256sum -c - > "$partial/sif-sha256.log"

{
  echo "host=$(hostname)"
  echo "jobId=$SLURM_JOB_ID"
  echo "submissionId=$SUBMISSION_ID"
  echo "runId=$RUN_ID"
  echo "replacesJob=$REPLACES_JOB"
  echo "sif=$SIF"
  echo "sifSha256=sha256:$SIF_SHA256"
  echo "slurmMemory=5G"
  echo "runtime=fake"
  echo "stages=1"
  echo "modelLoad=disabled"
  echo "scratch=$scratch"
  id
  apptainer version
  df -Pk "$scratch"
} > "$partial/environment.txt"

apptainer exec --cleanenv --containall \
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
if grep -Eini "qwen|torch|cuda|is not an instance of 'none'" \
    "$scratch"/*.log >/dev/null 2>&1; then
  echo FORBIDDEN_MODEL_OR_BINDING_MARKER >&2
  exit 8
fi
test -z "$(find "$scratch" -type f \
  \( -name '*.privkey' -o -name 'bootstrap-tokens.txt' \) -print -quit)"

terminal_state='PASS'
