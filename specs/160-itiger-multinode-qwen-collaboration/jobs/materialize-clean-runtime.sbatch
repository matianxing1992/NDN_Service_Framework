#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=22G
#SBATCH --time=00:30:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec160/materialization/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec160/materialization/slurm-%j.err

set -Eeuo pipefail
umask 077

readonly OCI_REFERENCE='ghcr.io/matianxing1992/ndnsf-di@sha256:178122a30d0a29bac9ff1c9759667cffa9a7e9f6a969daefa5187726acc0c6f7'
readonly RELEASE_ID='spec160-178122a30d0a'
readonly RUN_ID='spec160-run-sif-178122a30d0a-001'
readonly SUBMISSION_ID='spec160-submission-sif-178122a30d0a-001'
readonly REPLACES_JOB='none'
readonly PROJECT_ROOT='/project/tma1/ndnsf-di'
readonly RELEASE_DIR="${PROJECT_ROOT}/releases/${RELEASE_ID}"
readonly EVIDENCE_PARENT="${PROJECT_ROOT}/evidence/spec160/materialization"
readonly MATERIALIZER="${PROJECT_ROOT}/src/spec160/materialize-sif.sh"

: "${SLURM_JOB_ID:?SLURM_JOB_ID_REQUIRED}"
test -x "$MATERIALIZER" || { echo MATERIALIZER_MISSING >&2; exit 4; }
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
    mv "$partial" "$final"
  fi
  exit "$rc"
}
trap finish EXIT INT TERM

base="${SLURM_TMPDIR:-/tmp/$USER}"
case "$(realpath -m "$base")" in
  /home|/home/*|/project|/project/*) base="/tmp/$USER" ;;
esac
mkdir -p "$base"
test "$(df -Pk "$base" | awk 'NR==2 {print $4}')" -ge 26214400 || {
  echo COMPUTE_SCRATCH_UNAVAILABLE_25G >&2
  exit 4
}
scratch=$(mktemp -d "$base/spec160-sif-${SLURM_JOB_ID}.XXXXXX")
export APPTAINER_CACHEDIR="$scratch/cache"
export APPTAINER_TMPDIR="$scratch/tmp"
export TMPDIR="$scratch/tmp"
mkdir -p "$APPTAINER_CACHEDIR" "$APPTAINER_TMPDIR" "$scratch/home"

{
  echo "host=$(hostname)"
  echo "jobId=$SLURM_JOB_ID"
  echo "submissionId=$SUBMISSION_ID"
  echo "runId=$RUN_ID"
  echo "replacesJob=$REPLACES_JOB"
  echo "ociReference=$OCI_REFERENCE"
  id
  getent passwd "$(id -u)"
  apptainer version
  df -Pk "$scratch"
} > "$partial/environment.txt"

"$MATERIALIZER" \
  --oci-reference "$OCI_REFERENCE" \
  --sif "$scratch/runtime.sif" \
  --record "$scratch/materialization-scratch.json" \
  > "$partial/materialize.log" 2>&1

apptainer exec --cleanenv --containall \
  --home "$scratch/home:/home/tma1" \
  --env PYTHONNOUSERSITE=1,PYTHONDONTWRITEBYTECODE=1 \
  --bind "$scratch:/scratch:rw" \
  "$scratch/runtime.sif" \
  /usr/local/bin/ndnsf-di-probe-runtime --mode static \
  --output /scratch/static-probe.json > "$partial/static-probe.log" 2>&1

cp "$scratch/static-probe.json" "$partial/static-probe.json"
cp "$scratch/materialization-scratch.json" "$partial/materialization-scratch.json"
apptainer inspect "$scratch/runtime.sif" > "$partial/apptainer-inspect.json"
sif_sha=$(sha256sum "$scratch/runtime.sif" | awk '{print $1}')
printf '%s  %s\n' "$sif_sha" "$scratch/runtime.sif" > "$partial/runtime.sif.sha256"

mkdir -p "$RELEASE_DIR"
if [ -e "$RELEASE_DIR/runtime.sif" ] || \
   [ -e "$RELEASE_DIR/materialization.json" ]; then
  echo DURABLE_RELEASE_EXISTS >&2
  exit 4
fi
cp "$scratch/runtime.sif" "$RELEASE_DIR/.runtime.sif.$SLURM_JOB_ID.partial"
chmod 0444 "$RELEASE_DIR/.runtime.sif.$SLURM_JOB_ID.partial"
sync "$RELEASE_DIR/.runtime.sif.$SLURM_JOB_ID.partial"
ln "$RELEASE_DIR/.runtime.sif.$SLURM_JOB_ID.partial" "$RELEASE_DIR/runtime.sif"
rm "$RELEASE_DIR/.runtime.sif.$SLURM_JOB_ID.partial"

python3 - "$RELEASE_DIR/.materialization.json.$SLURM_JOB_ID.partial" \
  "$OCI_REFERENCE" "$sif_sha" "$RELEASE_DIR/runtime.sif" \
  "$SUBMISSION_ID" "$SLURM_JOB_ID" "$REPLACES_JOB" <<'PY'
import hashlib,json,subprocess,sys
path,oci,sif_sha,sif,submission,job,replaces=sys.argv[1:]
body={'schemaVersion':'spec160-sif-promotion-v1','ociReference':oci,
      'ociDigest':'sha256:'+oci.rsplit('@sha256:',1)[1],
      'sifPath':sif,'sifSha256':'sha256:'+sif_sha,
      'apptainerVersion':subprocess.check_output(
          ['apptainer','version'],text=True).strip(),
      'submissionId':submission,'slurmJobId':job,'replacesJob':replaces,
      'verified':True,'builtInAllocationScratch':True,'atomicPromotion':True}
body['recordDigest']='sha256:'+hashlib.sha256(
    json.dumps(body,sort_keys=True,separators=(',',':')).encode()).hexdigest()
with open(path,'x',encoding='utf-8') as stream:
  json.dump(body,stream,indent=2,sort_keys=True)
  stream.write('\n')
PY
chmod 0444 "$RELEASE_DIR/.materialization.json.$SLURM_JOB_ID.partial"
sync "$RELEASE_DIR/.materialization.json.$SLURM_JOB_ID.partial"
ln "$RELEASE_DIR/.materialization.json.$SLURM_JOB_ID.partial" \
  "$RELEASE_DIR/materialization.json"
rm "$RELEASE_DIR/.materialization.json.$SLURM_JOB_ID.partial"

sha256sum "$RELEASE_DIR/runtime.sif" > "$partial/durable-runtime.sif.sha256"
cp "$RELEASE_DIR/materialization.json" "$partial/materialization.json"
find "$partial" -maxdepth 1 -type f -print0 | sort -z | xargs -0 sha256sum \
  > "$partial/checksums.sha256"
terminal_state='PASS'
