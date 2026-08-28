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
readonly IMAGE_HEX='639e1ee9bab5dfd3b300eb221e5fe1404922aa5d02e1aa45ed2e978879539520'
readonly ARCHIVE='/project/tma1/ndnsf-di/images/spec159-a0e58822fe127594/candidate-docker-archive.tar.gz'
readonly ARCHIVE_SHA256='16a8a14a34733accbeaf8109523e3eee9b9b3eafa3dca880fec860fd4307674e'
readonly RUN_ID='spec159-run-sif-archive-a0e58822fe127594-003'
readonly SUBMISSION_ID='spec159-submission-sif-archive-a0e58822fe127594-003'
readonly REPLACES_JOB='172721'
readonly PROJECT_ROOT='/project/tma1/ndnsf-di'
readonly RELEASE_DIR="${PROJECT_ROOT}/releases/spec159-a0e58822fe127594"
readonly EVIDENCE_PARENT="${PROJECT_ROOT}/evidence/spec159/materialization"

test -n "${SLURM_JOB_ID:-}" || { echo SLURM_JOB_ID_REQUIRED >&2; exit 4; }
test -f "$ARCHIVE" || { echo SEALED_ARCHIVE_MISSING >&2; exit 4; }
comment=$(scontrol show job "$SLURM_JOB_ID" -o | tr ' ' '\n' | sed -n 's/^Comment=//p')
test "$comment" = "spec159:${SUBMISSION_ID}" || { echo SUBMISSION_COMMENT_INVALID >&2; exit 4; }

partial="${EVIDENCE_PARENT}/.${SUBMISSION_ID}.partial"
final="${EVIDENCE_PARENT}/${SUBMISSION_ID}"
test ! -e "$partial" && test ! -e "$final" || { echo EVIDENCE_IDENTITY_EXISTS >&2; exit 4; }
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
test "$(df -Pk "$base" | awk 'NR==2 {print $4}')" -ge 26214400 || {
  echo COMPUTE_SCRATCH_UNAVAILABLE_25G >&2; exit 4;
}
scratch=$(mktemp -d "$base/spec159-sif-${SLURM_JOB_ID}.XXXXXX")
export APPTAINER_CACHEDIR="$scratch/cache" APPTAINER_TMPDIR="$scratch/tmp" TMPDIR="$scratch/tmp"
mkdir -p "$APPTAINER_CACHEDIR" "$APPTAINER_TMPDIR"

{
  echo "host=$(hostname)"
  echo "jobId=$SLURM_JOB_ID"
  echo "submissionId=$SUBMISSION_ID"
  echo "runId=$RUN_ID"
  echo "replacesFailedJob=$REPLACES_JOB"
  echo "ociReference=$OCI_REFERENCE"
  echo "localImageId=sha256:$IMAGE_HEX"
  echo "archive=$ARCHIVE"
  id
  getent passwd "$(id -u)"
  apptainer version
  df -Pk "$scratch"
} > "$partial/environment.txt"

printf '%s  %s\n' "$ARCHIVE_SHA256" "$ARCHIVE" | sha256sum -c - > "$partial/archive-sha256.log"
gzip -t "$ARCHIVE"
gzip -dc "$ARCHIVE" > "$scratch/candidate.tar"
tar -tf "$scratch/candidate.tar" > "$partial/archive-members.txt"
grep -Fxq "blobs/sha256/$IMAGE_HEX" "$partial/archive-members.txt" || {
  echo ARCHIVE_OCI_CONFIG_MISSING >&2; exit 4;
}

apptainer build "$scratch/runtime.sif" "docker-archive:$scratch/candidate.tar" \
  > "$partial/materialize.log" 2>&1
test -s "$scratch/runtime.sif" || { echo MATERIALIZATION_EMPTY_SIF >&2; exit 4; }
apptainer exec --cleanenv --containall --no-home "$scratch/runtime.sif" \
  /usr/local/bin/ndnsf-di-probe-runtime --mode static \
  --output "$scratch/static-probe.json" > "$partial/static-probe.log" 2>&1
cp "$scratch/static-probe.json" "$partial/static-probe.json"
apptainer inspect "$scratch/runtime.sif" > "$partial/apptainer-inspect.json"
sif_sha=$(sha256sum "$scratch/runtime.sif" | awk '{print $1}')
printf '%s  %s\n' "$sif_sha" "$scratch/runtime.sif" > "$partial/runtime.sif.sha256"

mkdir -p "$RELEASE_DIR"
test ! -e "$RELEASE_DIR/runtime.sif" && test ! -e "$RELEASE_DIR/materialization.json" || {
  echo DURABLE_RELEASE_EXISTS >&2; exit 4;
}
cp "$scratch/runtime.sif" "$RELEASE_DIR/.runtime.sif.$SLURM_JOB_ID.partial"
chmod 0444 "$RELEASE_DIR/.runtime.sif.$SLURM_JOB_ID.partial"
sync "$RELEASE_DIR/.runtime.sif.$SLURM_JOB_ID.partial"
ln "$RELEASE_DIR/.runtime.sif.$SLURM_JOB_ID.partial" "$RELEASE_DIR/runtime.sif"
rm "$RELEASE_DIR/.runtime.sif.$SLURM_JOB_ID.partial"

python3 - "$RELEASE_DIR/.materialization.json.$SLURM_JOB_ID.partial" \
  "$OCI_REFERENCE" "$IMAGE_HEX" "$ARCHIVE_SHA256" "$sif_sha" \
  "$RELEASE_DIR/runtime.sif" "$SUBMISSION_ID" "$SLURM_JOB_ID" "$REPLACES_JOB" <<'PY'
import hashlib,json,subprocess,sys
path,oci,image,archive,sif_sha,sif,submission,job,replaces=sys.argv[1:]
body={'schemaVersion':'spec159-sif-promotion-v2','ociReference':oci,
 'ociDigest':'sha256:'+oci.rsplit('@sha256:',1)[1],'localImageId':'sha256:'+image,
 'sealedArchiveSha256':'sha256:'+archive,'sifPath':sif,'sifSha256':'sha256:'+sif_sha,
 'apptainerVersion':subprocess.check_output(['apptainer','version'],text=True).strip(),
 'submissionId':submission,'slurmJobId':job,'replacesFailedJob':replaces,
 'verified':True,'builtInAllocationScratch':True,'atomicPromotion':True}
body['recordDigest']='sha256:'+hashlib.sha256(
 json.dumps(body,sort_keys=True,separators=(',',':')).encode()).hexdigest()
with open(path,'x',encoding='utf-8') as f: json.dump(body,f,indent=2,sort_keys=True);f.write('\n')
PY
chmod 0444 "$RELEASE_DIR/.materialization.json.$SLURM_JOB_ID.partial"
sync "$RELEASE_DIR/.materialization.json.$SLURM_JOB_ID.partial"
ln "$RELEASE_DIR/.materialization.json.$SLURM_JOB_ID.partial" "$RELEASE_DIR/materialization.json"
rm "$RELEASE_DIR/.materialization.json.$SLURM_JOB_ID.partial"

sha256sum "$RELEASE_DIR/runtime.sif" > "$partial/durable-runtime.sif.sha256"
cp "$RELEASE_DIR/materialization.json" "$partial/materialization.json"
find "$partial" -maxdepth 1 -type f -print0 | sort -z | xargs -0 sha256sum > "$partial/checksums.sha256"
terminal_state='PASS'
