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
readonly RUN_ID='spec160-run-operation-status-sif-smoke-af0c3a3ce357-001'
readonly SUBMISSION_ID='spec160-submission-operation-status-sif-smoke-af0c3a3ce357-001'
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
mkdir -p "$partial/home"

terminal_state='FAIL'
finish()
{
  rc=$?
  trap - EXIT INT TERM
  python3 - "$partial/result.json" "$terminal_state" "$rc" "$SLURM_JOB_ID" \
    "$SUBMISSION_ID" "$RUN_ID" <<'PY' || true
import json,sys
path,state,rc,job,submission,run=sys.argv[1:]
with open(path,'w',encoding='utf-8') as stream:
  json.dump({'schemaVersion':'spec160-slurm-terminal-v1','state':state,
             'exitCode':int(rc),'jobId':job,'submissionId':submission,
             'runId':run},stream,indent=2,sort_keys=True)
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
  echo "sif=$SIF"
  echo "sifSha256=sha256:$SIF_SHA256"
  echo "slurmMemory=5G"
  echo "runtime=fake"
  echo "stages=1"
  echo "modelLoad=disabled"
  id
  apptainer version
} > "$partial/environment.txt"

/usr/bin/time -v -o "$partial/resource-usage.txt" \
  apptainer exec --cleanenv --containall \
    --home "$partial/home:/home/tma1" \
    --bind "$partial:/scratch:rw" \
    --bind "$SOURCE_ROOT:/harness:ro" \
    --bind "$SOURCE_ROOT/llm_pipeline:/source/llm_pipeline:ro" \
    --env PYTHONNOUSERSITE=1,PYTHONDONTWRITEBYTECODE=1 \
    "$SIF" /harness/local-docker-operation-status-inner.sh \
    > "$partial/smoke.log" 2>&1

grep -Fq SPEC160_LOCAL_DOCKER_OPERATION_STATUS_SMOKE_PASS "$partial/smoke.log"
grep -Fq LLM_PIPELINE_STAGE_FINAL "$partial/provider.log"
awk -F, 'NR > 1 && $1 == "measured" && $4 == "ok" { found = 1 } END { exit !found }' \
  "$partial/user-metrics.csv"
if grep -Eini "qwen|torch|cuda|is not an instance of 'none'" \
    "$partial"/*.log >/dev/null 2>&1; then
  echo FORBIDDEN_MODEL_OR_BINDING_MARKER >&2
  exit 8
fi
test -z "$(find "$partial" -type f \
  \( -name '*.privkey' -o -name 'bootstrap-tokens.txt' \) -print -quit)"

terminal_state='PASS'
