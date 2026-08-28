#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=24G
#SBATCH --time=00:15:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec160/runtime-smoke/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec160/runtime-smoke/slurm-%j.err

set -Eeuo pipefail
umask 077

readonly SIF='/project/tma1/ndnsf-di/releases/spec160-178122a30d0a-archive/runtime.sif'
readonly SIF_SHA256='8d8d95e7a5de86036ad62d972b911a66f2160553c6b3295d029dd3e7ae26106f'
readonly RUN_ID='spec160-run-runtime-smoke-178122a30d0a-003'
readonly SUBMISSION_ID='spec160-submission-runtime-smoke-178122a30d0a-003'
readonly REPLACES_JOB='173296'
readonly PROJECT_ROOT='/project/tma1/ndnsf-di'
readonly EVIDENCE_PARENT="${PROJECT_ROOT}/evidence/spec160/runtime-smoke"

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

scratch=''
terminal_state='FAIL'
finish()
{
  rc=$?
  trap - EXIT INT TERM
  if [ -n "$scratch" ] && [ -d "$scratch/log" ]; then
    cp -a "$scratch/log/." "$partial/" 2>/dev/null || true
  fi
  [ -z "$scratch" ] || [ ! -d "$scratch" ] || rm -rf "$scratch"
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
    find "$partial" -maxdepth 1 -type f -print0 | sort -z | xargs -0 sha256sum \
      > "$partial/checksums.sha256"
    mv "$partial" "$final"
  fi
  exit "$rc"
}
trap finish EXIT INT TERM

printf '%s  %s\n' "$SIF_SHA256" "$SIF" | sha256sum -c - > "$partial/sif-sha256.log"

base="${SLURM_TMPDIR:-/tmp/$USER}"
case "$(realpath -m "$base")" in
  /home|/home/*|/project|/project/*) base="/tmp/$USER" ;;
esac
mkdir -p "$base"
scratch=$(mktemp -d "$base/spec160-runtime-smoke-${SLURM_JOB_ID}.XXXXXX")
mkdir -p "$scratch/run" "$scratch/home" "$scratch/log" "$scratch/source/examples"

cp /project/tma1/ndnsf-di/src/spec160/runtime-smoke-inner.sh "$scratch/source/runtime-smoke-inner.sh"
cp /project/tma1/ndnsf-di/src/spec160/python_token_certificate_bootstrap_smoke.py "$scratch/source/examples/python_token_certificate_bootstrap_smoke.py"
cp /project/tma1/ndnsf-di/src/spec160/hello.policies "$scratch/source/examples/hello.policies"
cp /project/tma1/ndnsf-di/src/spec160/hello.bootstrap-tokens "$scratch/source/examples/hello.bootstrap-tokens"
cp /project/tma1/ndnsf-di/src/spec160/trust-schema.conf "$scratch/source/examples/trust-schema.conf"
chmod 700 "$scratch/source/runtime-smoke-inner.sh"

sed -e 's|@@NFD_SOCKET@@|/scratch/run/nfd.sock|g' \
    -e "s|@@PORT@@|6363|g" \
    /project/tma1/ndnsf-di/src/spec160/nfd.conf.in > "$scratch/nfd.conf"

{
  echo "host=$(hostname)"
  echo "jobId=$SLURM_JOB_ID"
  echo "submissionId=$SUBMISSION_ID"
  echo "runId=$RUN_ID"
  echo "replacesJob=$REPLACES_JOB"
  echo "sif=$SIF"
  echo "sifSha256=sha256:$SIF_SHA256"
  id
  apptainer version
  df -Pk "$scratch"
} > "$partial/environment.txt"

apptainer exec --cleanenv --containall \
  --home "$scratch/home:/home/tma1" \
  --bind "$scratch:/scratch:rw" \
  --bind "$scratch/source:/source:ro" \
  --env "SLURM_JOB_ID=${SLURM_JOB_ID}" \
  "$SIF" /source/runtime-smoke-inner.sh > "$partial/runtime-smoke.log" 2>&1

terminal_state='PASS'
