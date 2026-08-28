#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=256G
#SBATCH --gres=gpu:h100_80gb:1
#SBATCH --time=04:00:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec161/qwen32b-prep/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec161/qwen32b-prep/slurm-%j.err

set -Eeuo pipefail
umask 077

: "${SPEC161_PREP_RUN_ID:?}"
: "${SPEC161_PREP_SUBMISSION_ID:?}"
: "${SPEC161_PREP_SOURCE_ROOT:?}"
: "${SPEC161_PREP_SOURCE_SHA256:?}"
: "${SPEC161_SIF:?}"
: "${SPEC161_SIF_SHA256:?}"
: "${SPEC161_CAPACITY_DECISION:?}"
: "${SPEC161_CAPACITY_DECISION_SHA256:?}"

readonly evidence_parent=/project/tma1/ndnsf-di/evidence/spec161/qwen32b-prep
readonly artifact_parent=/project/tma1/ndnsf-di/artifacts/spec161/qwen32b
readonly partial="${evidence_parent}/.${SPEC161_PREP_SUBMISSION_ID}.partial"
readonly final="${evidence_parent}/${SPEC161_PREP_SUBMISSION_ID}"
readonly artifact_partial="${artifact_parent}/.${SPEC161_PREP_SUBMISSION_ID}.partial"
readonly artifact_final="${artifact_parent}/${SPEC161_PREP_SUBMISSION_ID}"
test ! -e "$partial"
test ! -e "$final"
test ! -e "$artifact_partial"
test ! -e "$artifact_final"
mkdir -p "$partial/source" "$artifact_partial"

cp -a "$SPEC161_PREP_SOURCE_ROOT"/jobs "$partial/source/"
cp -a "$SPEC161_PREP_SOURCE_ROOT"/contracts "$partial/source/"
cp -a "$SPEC161_PREP_SOURCE_ROOT"/llm_pipeline "$partial/source/"
cp "$SPEC161_CAPACITY_DECISION" "$partial/capacity-decision.json"
printf '%s  source-bundle\n' "$SPEC161_PREP_SOURCE_SHA256" \
  > "$partial/source-bundle.sha256"
printf '%s  %s\n' "$SPEC161_SIF_SHA256" "$SPEC161_SIF" |
  sha256sum -c - > "$partial/sif-sha256.log"
printf '%s  %s\n' "$SPEC161_CAPACITY_DECISION_SHA256" \
  "$partial/capacity-decision.json" |
  sha256sum -c - > "$partial/capacity-decision-sha256.log"
hostname > "$partial/hostname.txt"
nvidia-smi --query-gpu=uuid,name,driver_version,memory.total \
  --format=csv,noheader > "$partial/gpu.csv"
test "$(wc -l < "$partial/gpu.csv")" -eq 1
grep -qi h100 "$partial/gpu.csv"

terminal_state=FAIL
cleanup_performed=false
finish()
{
  rc=$?
  trap - EXIT INT TERM
  python3 - "$partial/result.json" "$terminal_state" "$rc" \
    "$cleanup_performed" <<'PY' || true
import json
import os
import sys

path, state, return_code, cleanup = sys.argv[1:]
with open(path, "w", encoding="utf-8") as stream:
    json.dump({
        "schemaVersion": "ndnsf-di-qwen32b-preparation-result-v1",
        "state": state,
        "exitCode": int(return_code),
        "jobId": os.environ["SLURM_JOB_ID"],
        "runId": os.environ["SPEC161_PREP_RUN_ID"],
        "submissionId": os.environ["SPEC161_PREP_SUBMISSION_ID"],
        "sourceSha256": os.environ["SPEC161_PREP_SOURCE_SHA256"],
        "sifSha256": os.environ["SPEC161_SIF_SHA256"],
        "capacityDecisionSha256":
            os.environ["SPEC161_CAPACITY_DECISION_SHA256"],
        "jobScratchCleanupPerformed": cleanup == "true",
        "modelSourcePromoted": False,
    }, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY
  (
    cd "$partial"
    find . -type f ! -name checksums.sha256 -print0 |
      sort -z |
      xargs -0 sha256sum
  ) > "$partial/checksums.sha256" 2>/dev/null || true
  if test "$terminal_state" = PASS && test "$rc" -eq 0; then
    mv "$partial" "$final"
  fi
  exit "$rc"
}
trap finish EXIT INT TERM

comment=$(scontrol show job "$SLURM_JOB_ID" -o |
  tr ' ' '\n' | sed -n 's/^Comment=//p')
test "$comment" = "spec161:${SPEC161_PREP_SUBMISSION_ID}"

python3 - "$SPEC161_CAPACITY_DECISION" <<'PY'
import json
import sys
doc = json.load(open(sys.argv[1], encoding="utf-8"))
if doc.get("allowed") is not True or doc.get("blockReasons"):
    raise SystemExit("SPEC161_CAPACITY_DECISION_NOT_ALLOWED")
if doc.get("cleanupAuthorized") is not False:
    raise SystemExit("SPEC161_UNEXPECTED_CLEANUP_AUTHORITY")
PY

scratch_root=""
for candidate in "${SLURM_TMPDIR:-}" /scratch /tmp; do
  test -n "$candidate" || continue
  test -d "$candidate" || continue
  test -w "$candidate" || continue
  scratch_root=$candidate
  break
done
test -n "$scratch_root"
scratch_prefix="${scratch_root%/}/tma1/ndnsf-di/spec161-prep-${SLURM_JOB_ID}"
case "$scratch_prefix" in
  /scratch/tma1/ndnsf-di/spec161-prep-*|/tmp/tma1/ndnsf-di/spec161-prep-*) ;;
  *)
    if test -z "${SLURM_TMPDIR:-}" ||
       test "$scratch_prefix" != \
         "${SLURM_TMPDIR%/}/tma1/ndnsf-di/spec161-prep-${SLURM_JOB_ID}"; then
      echo "UNSAFE_SCRATCH_PREFIX:$scratch_prefix" >&2
      exit 4
    fi
    ;;
esac
test ! -e "$scratch_prefix"
mkdir -p "$scratch_prefix/home"
df -B1 "$scratch_prefix" > "$partial/scratch-df-before.txt"
findmnt -T "$scratch_prefix" > "$partial/scratch-findmnt.txt"

python3 - "$scratch_prefix" <<'PY'
import shutil
import sys
required = 158119221002
usage = shutil.disk_usage(sys.argv[1])
if usage.free < required:
    raise SystemExit(
        f"SPEC161_SCRATCH_CAPACITY_FAIL:{usage.free}<{required}")
print(f"SPEC161_SCRATCH_CAPACITY_OK free={usage.free} required={required}")
PY

export SPEC161_PREP_WORK_ROOT="$scratch_prefix"
export SPEC161_PROMOTED_ARTIFACT_DIR="$artifact_final"
apptainer exec --nv --cleanenv --containall \
  --home "$scratch_prefix/home:/home/${USER}" \
  --bind "$scratch_prefix:/work:rw" \
  --bind "$SPEC161_PREP_SOURCE_ROOT:/source:ro" \
  --env "PYTHONPATH=/source/llm_pipeline" \
  --env "HF_HOME=/work/huggingface-home" \
  --env "HF_HUB_DISABLE_TELEMETRY=1" \
  --env "TOKENIZERS_PARALLELISM=false" \
  --env "SPEC161_PREP_WORK_ROOT=/work" \
  --env "SPEC161_PREP_SOURCE_ROOT=/source" \
  --env "SPEC161_PROMOTED_ARTIFACT_DIR=$artifact_final" \
  --env "SPEC161_PREP_SUBMISSION_ID=$SPEC161_PREP_SUBMISSION_ID" \
  --env "SPEC161_CAPACITY_DECISION_SHA256=$SPEC161_CAPACITY_DECISION_SHA256" \
  --env "SPEC161_SIF_SHA256=$SPEC161_SIF_SHA256" \
  --env "SPEC161_PREP_SOURCE_SHA256=$SPEC161_PREP_SOURCE_SHA256" \
  "$SPEC161_SIF" \
  bash /source/jobs/prepare-stages.sbatch \
  > "$partial/preparation.log" 2>&1

output="$scratch_prefix/output"
test -f "$output/stage-manifest.json"
test -f "$output/reference.json"
test -f "$output/tokenizer-manifest.json"
test -f "$output/smoke-campaign.json"
test -f "$output/policy.yaml"
test -f "$output/model-source-manifest.json"
test "$(find "$output/qwen-transformers-stage-artifacts" \
  -maxdepth 1 -type f -name 'stage-*-qwen-transformers.pt' | wc -l)" -eq 3

current_bytes=$(du -sx --block-size=1 /project/tma1/ndnsf-di |
  awk '{print $1}')
promotion_bytes=$(du -sx --block-size=1 "$output" | awk '{print $1}')
reserve_bytes=21474836480
quota_bytes=1000000000000
projected_bytes=$((current_bytes + promotion_bytes + reserve_bytes))
{
  echo "currentBytes=$current_bytes"
  echo "promotionBytes=$promotion_bytes"
  echo "reserveBytes=$reserve_bytes"
  echo "quotaBytes=$quota_bytes"
  echo "projectedWithReserveBytes=$projected_bytes"
} > "$partial/promotion-capacity.txt"
test "$projected_bytes" -le "$quota_bytes"

cp -a "$output/." "$artifact_partial/"
test ! -e "$artifact_partial/huggingface-cache"
test ! -e "$artifact_partial/model"
touch "$artifact_partial/ACCEPTED"
(
  cd "$artifact_partial"
  find . -type f ! -name artifact-checksums.sha256 -print0 |
    sort -z |
    xargs -0 sha256sum
) > "$artifact_partial/artifact-checksums.sha256"
cp "$artifact_partial/stage-manifest.json" "$partial/stage-manifest.json"
cp "$artifact_partial/reference.json" "$partial/reference.json"
cp "$artifact_partial/tokenizer-manifest.json" "$partial/tokenizer-manifest.json"
cp "$artifact_partial/smoke-campaign.json" "$partial/smoke-campaign.json"
cp "$artifact_partial/model-source-manifest.json" \
  "$partial/model-source-manifest.json"
cp "$artifact_partial/artifact-checksums.sha256" \
  "$partial/artifact-checksums.sha256"
find "$artifact_partial" -type f -exec chmod 400 {} +
find "$artifact_partial" -type d -exec chmod 500 {} +
mv "$artifact_partial" "$artifact_final"

df -B1 "$scratch_prefix" > "$partial/scratch-df-after.txt"
find "$scratch_prefix" -mindepth 1 -delete
rmdir -- "$scratch_prefix"
cleanup_performed=true
terminal_state=PASS
