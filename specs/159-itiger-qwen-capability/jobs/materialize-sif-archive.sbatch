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

readonly OCI_REFERENCE='ghcr.io/matianxing1992/ndnsf-di-spec159@sha256:47f9fa10cdea67e622694c8c984ad82a6e8f270e6cdbf82e531b9561513b70e8'
readonly LOCAL_IMAGE_ID='sha256:639e1ee9bab5dfd3b300eb221e5fe1404922aa5d02e1aa45ed2e978879539520'
readonly ARCHIVE='/project/tma1/ndnsf-di/images/spec159-a0e58822fe127594/candidate-docker-archive.tar.gz'
readonly ARCHIVE_SHA256='16a8a14a34733accbeaf8109523e3eee9b9b3eafa3dca880fec860fd4307674e'
readonly RELEASE_ID='spec159-a0e58822fe127594'
readonly RUN_ID='spec159-run-sif-archive-a0e58822fe127594-002'
readonly SUBMISSION_ID='spec159-submission-sif-archive-a0e58822fe127594-002'
readonly REPLACES_JOB='172675'
readonly PROJECT_ROOT='/project/tma1/ndnsf-di'
readonly RELEASE_DIR="${PROJECT_ROOT}/releases/${RELEASE_ID}"
readonly EVIDENCE_PARENT="${PROJECT_ROOT}/evidence/spec159/materialization"

test -n "${SLURM_JOB_ID:-}" || { echo SLURM_JOB_ID_REQUIRED >&2; exit 4; }
test -f "$ARCHIVE" || { echo SEALED_ARCHIVE_MISSING >&2; exit 4; }
observed_comment=$(scontrol show job "$SLURM_JOB_ID" -o | tr ' ' '\n' | sed -n 's/^Comment=//p')
test "$observed_comment" = "spec159:${SUBMISSION_ID}" || {
  echo "SUBMISSION_COMMENT_INVALID:${observed_comment}" >&2; exit 4;
}

evidence_partial="${EVIDENCE_PARENT}/.${SUBMISSION_ID}.partial"
evidence_final="${EVIDENCE_PARENT}/${SUBMISSION_ID}"
test ! -e "$evidence_partial" && test ! -e "$evidence_final" || {
  echo EVIDENCE_IDENTITY_EXISTS >&2; exit 4;
}
mkdir -p "$EVIDENCE_PARENT" "$evidence_partial"

scratch=''
terminal_state='FAIL'
finish()
{
  rc=$?
  trap - EXIT INT TERM
  if [ -n "$scratch" ] && [ -d "$scratch" ]; then rm -rf "$scratch"; fi
  python3 - "$evidence_partial/result.json" "$terminal_state" "$rc" \
    "$SLURM_JOB_ID" "$SUBMISSION_ID" "$RUN_ID" "$REPLACES_JOB" <<'PY' || true
import json,sys
path,state,rc,job,submission,run,replaces=sys.argv[1:]
with open(path,'w',encoding='utf-8') as stream:
  json.dump({'schemaVersion':'spec159-slurm-terminal-v1','state':state,
             'exitCode':int(rc),'jobId':job,'submissionId':submission,
             'runId':run,'replacesFailedJob':replaces},
            stream,indent=2,sort_keys=True);stream.write('\n')
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
  case "$resolved" in /home|/home/*|/project|/project/*) continue ;; esac
  available_kib=$(df -Pk "$resolved" | awk 'NR==2 {print $4}')
  [ "${available_kib:-0}" -ge 26214400 ] || continue
  scratch=$(mktemp -d "${resolved}/spec159-sif-archive-${SLURM_JOB_ID}.XXXXXX")
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
  echo "runId=$RUN_ID"
  echo "replacesFailedJob=$REPLACES_JOB"
  echo "scratch=$scratch"
  echo "ociReference=$OCI_REFERENCE"
  echo "localImageId=$LOCAL_IMAGE_ID"
  echo "archive=$ARCHIVE"
  id
  getent passwd "$(id -u)"
  apptainer version
  df -Pk "$scratch"
} > "$evidence_partial/environment.txt"

printf '%s  %s\n' "$ARCHIVE_SHA256" "$ARCHIVE" | sha256sum -c - \
  > "$evidence_partial/archive-sha256.log"
gzip -t "$ARCHIVE"
gzip -dc "$ARCHIVE" > "$scratch/candidate-docker-archive.tar"
tar -tf "$scratch/candidate-docker-archive.tar" > "$evidence_partial/archive-members.txt"
grep -Fxq "${LOCAL_IMAGE_ID#sha256:}.json" "$evidence_partial/archive-members.txt" || {
  echo ARCHIVE_LOCAL_IMAGE_CONFIG_MISSING >&2; exit 4;
}

apptainer build "$scratch/runtime.sif" \
  "docker-archive:$scratch/candidate-docker-archive.tar" \
  > "$evidence_partial/materialize.log" 2>&1
test -s "$scratch/runtime.sif" || { echo MATERIALIZATION_EMPTY_SIF >&2; exit 4; }

apptainer exec --cleanenv --containall --no-home "$scratch/runtime.sif" \
  /usr/local/bin/ndnsf-di-probe-runtime --mode static \
  --output "$scratch/static-probe.json" \
  > "$evidence_partial/static-probe.log" 2>&1

sha256sum "$scratch/runtime.sif" > "$evidence_partial/runtime.sif.sha256"
apptainer inspect "$scratch/runtime.sif" > "$evidence_partial/apptainer-inspect.json"
cp "$scratch/static-probe.json" "$evidence_partial/static-probe.json"

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

python3 - "$record_partial" "$OCI_REFERENCE" "$LOCAL_IMAGE_ID" \
  "$ARCHIVE_SHA256" "$RELEASE_DIR/runtime.sif" "$SUBMISSION_ID" \
  "$SLURM_JOB_ID" "$REPLACES_JOB" "$(apptainer version)" <<'PY'
import hashlib,json,sys
path,oci,image,archive_sha,sif,submission,job,replaces,version=sys.argv[1:]
digest=hashlib.sha256()
with open(sif,'rb') as stream:
  for chunk in iter(lambda:stream.read(1024*1024),b''): digest.update(chunk)
body={'schemaVersion':'spec159-sif-promotion-v2','ociReference':oci,
      'ociDigest':'sha256:'+oci.rsplit('@sha256:',1)[1],
      'localImageId':image,'sealedArchiveSha256':'sha256:'+archive_sha,
      'sifPath':sif,'sifSha256':'sha256:'+digest.hexdigest(),
      'apptainerVersion':version,'submissionId':submission,'slurmJobId':job,
      'replacesFailedJob':replaces,'verified':True,
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
