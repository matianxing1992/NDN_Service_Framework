#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=22G
#SBATCH --time=00:30:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec162/materialization/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec162/materialization/slurm-%j.err

set -Eeuo pipefail
umask 077

readonly OCI_REFERENCE='ghcr.io/matianxing1992/ndnsf-di@sha256:6216ef2b9525740423d67b5327933b59de284b01930aeccde3a657b3c0288f29'
readonly IMAGE_HEX='59c71dedf8736daf2e363b4b10b8b113c63c5d50ee0fe83619b283975eaf87c7'
readonly ARCHIVE='/project/tma1/ndnsf-di/images/spec162-6216ef2b9525/candidate-docker-archive.tar.gz'
readonly ARCHIVE_SHA256='bfac393e381f43ff954e49a70d48adc4a05f11fdf2584f58498f6f1f4f51254b'
readonly RELEASE_ID='spec162-6216ef2b9525'
readonly RELEASE_DIR="/project/tma1/ndnsf-di/releases/${RELEASE_ID}"
readonly SUBMISSION_ID='spec162-submission-sif-6216ef2b9525-001'
readonly RUN_ID='spec162-run-sif-6216ef2b9525-001'
readonly EVIDENCE_PARENT='/project/tma1/ndnsf-di/evidence/spec162/materialization'

: "${SLURM_JOB_ID:?SLURM_JOB_ID_REQUIRED}"
comment=$(scontrol show job "$SLURM_JOB_ID" -o | tr ' ' '\n' | sed -n 's/^Comment=//p')
test "$comment" = "spec162:${SUBMISSION_ID}" || {
  echo SUBMISSION_COMMENT_INVALID >&2
  exit 4
}

partial="${EVIDENCE_PARENT}/.${SUBMISSION_ID}.partial"
final="${EVIDENCE_PARENT}/${SUBMISSION_ID}"
test ! -e "$partial" && test ! -e "$final" && test ! -e "$RELEASE_DIR"
mkdir -p "$partial"

scratch=''
terminal_state='FAIL'
finish()
{
  rc=$?
  trap - EXIT INT TERM
  test -z "$scratch" || test ! -d "$scratch" || rm -rf -- "$scratch"
  python3 - "$partial/result.json" "$terminal_state" "$rc" "$SLURM_JOB_ID" <<'PY' || true
import json, sys
path, state, rc, job = sys.argv[1:]
with open(path, "w", encoding="utf-8") as stream:
    json.dump({
        "schemaVersion": "spec162-slurm-terminal-v1",
        "state": state,
        "exitCode": int(rc),
        "jobId": job,
        "submissionId": "spec162-submission-sif-6216ef2b9525-001",
        "runId": "spec162-run-sif-6216ef2b9525-001",
    }, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY
  if test "$terminal_state" = PASS && test "$rc" -eq 0; then
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
test "$(df -Pk "$base" | awk 'NR==2 {print $4}')" -ge 26214400
scratch=$(mktemp -d "$base/spec162-sif-${SLURM_JOB_ID}.XXXXXX")
export APPTAINER_CACHEDIR="$scratch/cache"
export APPTAINER_TMPDIR="$scratch/tmp"
export TMPDIR="$scratch/tmp"
mkdir -p "$APPTAINER_CACHEDIR" "$APPTAINER_TMPDIR" "$scratch/home"

{
  echo "host=$(hostname)"
  echo "jobId=$SLURM_JOB_ID"
  echo "submissionId=$SUBMISSION_ID"
  echo "runId=$RUN_ID"
  echo "ociReference=$OCI_REFERENCE"
  echo "localImageId=sha256:$IMAGE_HEX"
  echo "archive=$ARCHIVE"
  echo "archiveSha256=sha256:$ARCHIVE_SHA256"
  echo "modelsIncluded=false"
  id
  apptainer version
  df -Pk "$scratch"
} > "$partial/environment.txt"

printf '%s  %s\n' "$ARCHIVE_SHA256" "$ARCHIVE" |
  sha256sum -c - > "$partial/archive-sha256.log"
gzip -t "$ARCHIVE"
gzip -dc "$ARCHIVE" > "$scratch/candidate.tar"
tar -tf "$scratch/candidate.tar" > "$partial/archive-members.txt"
grep -Eq "(^|/)${IMAGE_HEX}(\\.json)?$|blobs/sha256/${IMAGE_HEX}$" \
  "$partial/archive-members.txt"

apptainer build "$scratch/runtime.sif" "docker-archive:$scratch/candidate.tar" \
  > "$partial/materialize.log" 2>&1
test -s "$scratch/runtime.sif"

apptainer exec --cleanenv --containall \
  --home "$scratch/home:/home/tma1" \
  "$scratch/runtime.sif" \
  /usr/local/bin/ndnsf-di-verify-qwen36-runtime \
  --lock /opt/ndnsf-di/locks/qwen36-overlay.lock.json \
  > "$partial/qwen36-runtime-probe.json"
grep -Fq '"status": "PASS"' "$partial/qwen36-runtime-probe.json"

apptainer inspect "$scratch/runtime.sif" > "$partial/apptainer-inspect.json"
sif_sha=$(sha256sum "$scratch/runtime.sif" | awk '{print $1}')
printf '%s  %s\n' "$sif_sha" "$scratch/runtime.sif" \
  > "$partial/runtime.sif.sha256"

mkdir "$RELEASE_DIR"
cp "$scratch/runtime.sif" "$RELEASE_DIR/.runtime.sif.${SLURM_JOB_ID}.partial"
chmod 0444 "$RELEASE_DIR/.runtime.sif.${SLURM_JOB_ID}.partial"
sync "$RELEASE_DIR/.runtime.sif.${SLURM_JOB_ID}.partial"
mv "$RELEASE_DIR/.runtime.sif.${SLURM_JOB_ID}.partial" \
  "$RELEASE_DIR/runtime.sif"

python3 - "$RELEASE_DIR/materialization.json" "$sif_sha" "$SLURM_JOB_ID" <<'PY'
import json, sys
path, sif_sha, job = sys.argv[1:]
with open(path, "x", encoding="utf-8") as stream:
    json.dump({
        "schemaVersion": "spec162-sif-promotion-v1",
        "ociReference": "ghcr.io/matianxing1992/ndnsf-di@sha256:6216ef2b9525740423d67b5327933b59de284b01930aeccde3a657b3c0288f29",
        "ociDigest": "sha256:6216ef2b9525740423d67b5327933b59de284b01930aeccde3a657b3c0288f29",
        "localImageId": "sha256:59c71dedf8736daf2e363b4b10b8b113c63c5d50ee0fe83619b283975eaf87c7",
        "sealedArchiveSha256": "sha256:bfac393e381f43ff954e49a70d48adc4a05f11fdf2584f58498f6f1f4f51254b",
        "sifPath": "/project/tma1/ndnsf-di/releases/spec162-6216ef2b9525/runtime.sif",
        "sifSha256": "sha256:" + sif_sha,
        "submissionId": "spec162-submission-sif-6216ef2b9525-001",
        "slurmJobId": job,
        "verified": True,
        "builtInAllocationScratch": True,
        "atomicPromotion": True,
        "modelsIncluded": False,
    }, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY
chmod 0444 "$RELEASE_DIR/materialization.json"
cp "$RELEASE_DIR/materialization.json" "$partial/materialization.json"
sha256sum "$RELEASE_DIR/runtime.sif" > "$partial/durable-runtime.sif.sha256"
(
  cd "$partial"
  find . -maxdepth 1 -type f ! -name checksums.sha256 -print0 |
    sort -z |
    xargs -0 sha256sum
) > "$partial/checksums.sha256"
terminal_state='PASS'

