#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodelist=itiger07
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=16G
#SBATCH --time=00:30:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec159/materialization/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec159/materialization/slurm-%j.err

set -Eeuo pipefail
umask 077

readonly OCI_REFERENCE='ghcr.io/matianxing1992/ndnsf-di@sha256:a0e58822fe1275947fe060df85b555f63d69029ac70c1a6788aa4d77c2d8acd2'
readonly RELEASE_ID='spec159-a0e58822fe127594'
readonly CANDIDATE_ID='spec159-candidate-a0e58822fe127594'
readonly RUN_ID='spec159-run-sif-a0e58822fe127594-001'
readonly SUBMISSION_ID='spec159-submission-sif-a0e58822fe127594-001'
readonly PROJECT_ROOT='/project/tma1/ndnsf-di'
readonly RELEASE_DIR="${PROJECT_ROOT}/releases/${RELEASE_ID}"
readonly EVIDENCE_PARENT="${PROJECT_ROOT}/evidence/spec159/materialization"
readonly MATERIALIZER="${PROJECT_ROOT}/src/spec159/materialize-sif.sh"

test -n "${SLURM_JOB_ID:-}" || { echo SLURM_JOB_ID_REQUIRED >&2; exit 4; }
test -x "$MATERIALIZER" || { echo MATERIALIZER_MISSING >&2; exit 4; }

job_record=$(scontrol show job "$SLURM_JOB_ID" -o)
observed_comment=$(printf '%s\n' "$job_record" | tr ' ' '\n' | sed -n 's/^Comment=//p')
test "$observed_comment" = "spec159:${SUBMISSION_ID}" || {
  echo "SUBMISSION_COMMENT_INVALID:${observed_comment}" >&2
  exit 4
}

evidence_partial="${EVIDENCE_PARENT}/.${SUBMISSION_ID}.partial"
evidence_final="${EVIDENCE_PARENT}/${SUBMISSION_ID}"
if [ -e "$evidence_partial" ] || [ -e "$evidence_final" ]; then
  echo EVIDENCE_IDENTITY_EXISTS >&2
  exit 4
fi
mkdir -p "$EVIDENCE_PARENT" "$evidence_partial"

scratch=''
terminal_state='FAIL'
finish()
{
  rc=$?
  trap - EXIT INT TERM
  if [ -n "$scratch" ] && [ -d "$scratch" ]; then
    rm -rf "$scratch"
  fi
  python3 - "$evidence_partial/result.json" "$terminal_state" "$rc" \
    "$SLURM_JOB_ID" "$SUBMISSION_ID" "$RUN_ID" <<'PY' || true
import json,sys
path,state,rc,job,submission,run=sys.argv[1:]
with open(path,'w',encoding='utf-8') as stream:
  json.dump({'schemaVersion':'spec159-slurm-terminal-v1','state':state,
             'exitCode':int(rc),'jobId':job,'submissionId':submission,
             'runId':run},stream,indent=2,sort_keys=True)
  stream.write('\n')
PY
  if [ "$terminal_state" = PASS ] && [ "$rc" -eq 0 ]; then
    mv "$evidence_partial" "$evidence_final"
  fi
  exit "$rc"
}
trap finish EXIT INT TERM

for base in "${SLURM_TMPDIR:-}" "/scratch/$USER" "/tmp/$USER"; do
  [ -n "$base" ] || continue
  mkdir -p "$base" 2>/dev/null || continue
  resolved=$(realpath -m "$base")
  case "$resolved" in
    /home|/home/*|/project|/project/*) continue ;;
  esac
  available_kib=$(df -Pk "$resolved" | awk 'NR==2 {print $4}')
  [ "${available_kib:-0}" -ge 26214400 ] || continue
  scratch=$(mktemp -d "${resolved}/spec159-sif-${SLURM_JOB_ID}.XXXXXX")
  break
done
test -n "$scratch" || { echo COMPUTE_SCRATCH_UNAVAILABLE_25G >&2; exit 4; }

export APPTAINER_CACHEDIR="$scratch/apptainer-cache"
export APPTAINER_TMPDIR="$scratch/apptainer-tmp"
export TMPDIR="$scratch/tmp"
mkdir -p "$APPTAINER_CACHEDIR" "$APPTAINER_TMPDIR" "$TMPDIR"

{
  echo "host=$(hostname)"
  echo "jobId=$SLURM_JOB_ID"
  echo "submissionId=$SUBMISSION_ID"
  echo "candidateId=$CANDIDATE_ID"
  echo "runId=$RUN_ID"
  echo "scratch=$scratch"
  echo "ociReference=$OCI_REFERENCE"
  id
  getent passwd "$(id -u)"
  apptainer version
  df -Pk "$scratch"
} > "$evidence_partial/environment.txt"

"$MATERIALIZER" \
  --oci-reference "$OCI_REFERENCE" \
  --sif "$scratch/runtime.sif" \
  --record "$scratch/materialization-scratch.json" \
  > "$evidence_partial/materialize.log" 2>&1

apptainer exec --cleanenv --containall --no-home "$scratch/runtime.sif" \
  /usr/local/bin/ndnsf-di-probe-runtime --mode static \
  --output "$scratch/static-probe.json" \
  > "$evidence_partial/static-probe.log" 2>&1

sha256sum "$scratch/runtime.sif" > "$evidence_partial/runtime.sif.sha256"
apptainer inspect "$scratch/runtime.sif" > "$evidence_partial/apptainer-inspect.json"
cp "$scratch/static-probe.json" "$evidence_partial/static-probe.json"
cp "$scratch/materialization-scratch.json" "$evidence_partial/materialization-scratch.json"

mkdir -p "$RELEASE_DIR"
test ! -e "$RELEASE_DIR/runtime.sif" || { echo DURABLE_SIF_ALREADY_EXISTS >&2; exit 4; }
test ! -e "$RELEASE_DIR/materialization.json" || { echo DURABLE_RECORD_ALREADY_EXISTS >&2; exit 4; }

sif_partial="$RELEASE_DIR/.runtime.sif.${SLURM_JOB_ID}.partial"
record_partial="$RELEASE_DIR/.materialization.json.${SLURM_JOB_ID}.partial"
cp "$scratch/runtime.sif" "$sif_partial"
chmod 0444 "$sif_partial"
sync "$sif_partial"
ln "$sif_partial" "$RELEASE_DIR/runtime.sif"
rm "$sif_partial"

python3 - "$record_partial" "$OCI_REFERENCE" "$RELEASE_DIR/runtime.sif" \
  "$scratch/materialization-scratch.json" "$SUBMISSION_ID" "$SLURM_JOB_ID" <<'PY'
import hashlib,json,sys
path,oci,sif,scratch_record,submission,job=sys.argv[1:]
digest=hashlib.sha256()
with open(sif,'rb') as stream:
  for chunk in iter(lambda:stream.read(1024*1024),b''):
    digest.update(chunk)
source=json.load(open(scratch_record,encoding='utf-8'))
body={'schemaVersion':'spec159-sif-promotion-v1','ociReference':oci,
      'ociDigest':'sha256:'+oci.rsplit('@sha256:',1)[1],'sifPath':sif,
      'sifSha256':'sha256:'+digest.hexdigest(),'apptainerVersion':source['apptainerVersion'],
      'sourceMaterializationRecordDigest':source['recordDigest'],
      'submissionId':submission,'slurmJobId':job,'verified':True,
      'builtInAllocationScratch':True,'atomicPromotion':True}
body['recordDigest']='sha256:'+hashlib.sha256(
  json.dumps(body,sort_keys=True,separators=(',',':')).encode()).hexdigest()
with open(path,'x',encoding='utf-8') as stream:
  json.dump(body,stream,indent=2,sort_keys=True);stream.write('\n')
PY
chmod 0444 "$record_partial"
sync "$record_partial"
ln "$record_partial" "$RELEASE_DIR/materialization.json"
rm "$record_partial"

sha256sum "$RELEASE_DIR/runtime.sif" > "$evidence_partial/durable-runtime.sif.sha256"
cp "$RELEASE_DIR/materialization.json" "$evidence_partial/materialization.json"
find "$evidence_partial" -maxdepth 1 -type f -print0 | sort -z | xargs -0 sha256sum \
  > "$evidence_partial/checksums.sha256"
terminal_state='PASS'
