#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=256G
#SBATCH --gres=gpu:rtx_5000:3
#SBATCH --time=08:00:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec162/qwen36-prep/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec162/qwen36-prep/slurm-%j.err

set -Eeuo pipefail
umask 077

: "${SPEC162_PREP_RUN_ID:?}"
: "${SPEC162_PREP_SUBMISSION_ID:?}"
: "${SPEC162_PREP_SOURCE_ROOT:?}"
: "${SPEC162_PREP_SOURCE_SHA256:?}"
: "${SPEC162_SIF:?}"
: "${SPEC162_SIF_SHA256:?}"
: "${SPEC162_CAPACITY_DECISION:?}"
: "${SPEC162_CAPACITY_DECISION_SHA256:?}"

readonly model_profile="${SPEC162_MODEL_PROFILE:-qwen36-27b}"
case "$model_profile" in
  qwen36-27b) readonly model_tag=qwen36 ;;
  qwen3-0.6b) readonly model_tag=qwen3-0.6b ;;
  *) echo "SPEC162_MODEL_PROFILE_INVALID:$model_profile" >&2; exit 2 ;;
esac
readonly evidence_parent="/project/tma1/ndnsf-di/evidence/spec162/${model_tag}-prep"
readonly artifact_parent="/project/tma1/ndnsf-di/artifacts/spec162/${model_tag}"
readonly partial="${evidence_parent}/.${SPEC162_PREP_SUBMISSION_ID}.partial"
readonly final="${evidence_parent}/${SPEC162_PREP_SUBMISSION_ID}"
readonly artifact_partial="${artifact_parent}/.${SPEC162_PREP_SUBMISSION_ID}.partial"
readonly artifact_final="${artifact_parent}/${SPEC162_PREP_SUBMISSION_ID}"
test ! -e "$partial"
test ! -e "$final"
test ! -e "$artifact_partial"
test ! -e "$artifact_final"
mkdir -p "$partial/source" "$artifact_partial"

printf '%s  %s\n' "$SPEC162_PREP_SOURCE_SHA256" \
  "$SPEC162_PREP_SOURCE_ROOT/source-files.sha256" |
  sha256sum -c - > "$partial/source-manifest-sha256.log"
(
  cd "$SPEC162_PREP_SOURCE_ROOT"
  sha256sum -c source-files.sha256
) > "$partial/source-files-sha256.log"
cp -a "$SPEC162_PREP_SOURCE_ROOT"/jobs "$partial/source/"
cp -a "$SPEC162_PREP_SOURCE_ROOT"/contracts "$partial/source/"
cp -a "$SPEC162_PREP_SOURCE_ROOT"/llm_pipeline "$partial/source/"
cp "$SPEC162_PREP_SOURCE_ROOT/source-files.sha256" \
  "$partial/source/source-files.sha256"
cp "$SPEC162_CAPACITY_DECISION" "$partial/capacity-decision.json"
printf '%s  source-bundle\n' "$SPEC162_PREP_SOURCE_SHA256" \
  > "$partial/source-bundle.sha256"
printf '%s  %s\n' "$SPEC162_SIF_SHA256" "$SPEC162_SIF" |
  sha256sum -c - > "$partial/sif-sha256.log"
printf '%s  %s\n' "$SPEC162_CAPACITY_DECISION_SHA256" \
  "$partial/capacity-decision.json" |
  sha256sum -c - > "$partial/capacity-decision-sha256.log"
hostname > "$partial/hostname.txt"
nvidia-smi --query-gpu=index,uuid,name,driver_version,memory.total \
  --format=csv,noheader > "$partial/gpu.csv"
test "$(wc -l < "$partial/gpu.csv")" -eq 3
test "$(grep -ci 'RTX 5000' "$partial/gpu.csv")" -eq 3

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
        "schemaVersion": "ndnsf-di-qwen36-preparation-result-v1",
        "state": state,
        "exitCode": int(return_code),
        "jobId": os.environ["SLURM_JOB_ID"],
        "runId": os.environ["SPEC162_PREP_RUN_ID"],
        "submissionId": os.environ["SPEC162_PREP_SUBMISSION_ID"],
        "sourceSha256": os.environ["SPEC162_PREP_SOURCE_SHA256"],
        "sifSha256": os.environ["SPEC162_SIF_SHA256"],
        "capacityDecisionSha256":
            os.environ["SPEC162_CAPACITY_DECISION_SHA256"],
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
test "$comment" = "spec162:${SPEC162_PREP_SUBMISSION_ID}"

python3 - "$SPEC162_CAPACITY_DECISION" <<'PY'
import json
import sys
doc = json.load(open(sys.argv[1], encoding="utf-8"))
if doc.get("allowed") is not True or doc.get("blockReasons"):
    raise SystemExit("SPEC162_CAPACITY_DECISION_NOT_ALLOWED")
if doc.get("cleanupAuthorized") is not False:
    raise SystemExit("SPEC162_UNEXPECTED_CLEANUP_AUTHORITY")
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
scratch_prefix="${scratch_root%/}/tma1/ndnsf-di/spec162-prep-${SLURM_JOB_ID}"
case "$scratch_prefix" in
  /scratch/tma1/ndnsf-di/spec162-prep-*|/tmp/tma1/ndnsf-di/spec162-prep-*) ;;
  *)
    if test -z "${SLURM_TMPDIR:-}" ||
       test "$scratch_prefix" != \
         "${SLURM_TMPDIR%/}/tma1/ndnsf-di/spec162-prep-${SLURM_JOB_ID}"; then
      echo "UNSAFE_SCRATCH_PREFIX:$scratch_prefix" >&2
      exit 4
    fi
    ;;
esac
test ! -e "$scratch_prefix"
mkdir -p "$scratch_prefix/home"
df -B1 "$scratch_prefix" > "$partial/scratch-df-before.txt"
findmnt -T "$scratch_prefix" > "$partial/scratch-findmnt.txt"

python3 - "$scratch_prefix" "$SPEC162_CAPACITY_DECISION" <<'PY'
import json
import shutil
import sys
scratch, decision_path = sys.argv[1:]
decision = json.load(open(decision_path, encoding="utf-8"))
required = int(decision["scratchTemporaryPeakBytes"])
free = shutil.disk_usage(scratch).free
if free < required:
    raise SystemExit(f"SPEC162_SCRATCH_CAPACITY_FAIL:{free}<{required}")
print(f"SPEC162_SCRATCH_CAPACITY_OK free={free} required={required}")
PY

export SPEC162_PREP_WORK_ROOT="$scratch_prefix"
export SPEC162_PROMOTED_ARTIFACT_DIR="$artifact_final"
apptainer exec --nv --cleanenv --containall \
  --home "$scratch_prefix/home:/home/${USER}" \
  --bind "$scratch_prefix:/work:rw" \
  --bind "$SPEC162_PREP_SOURCE_ROOT:/source:ro" \
  --env "PYTHONPATH=/source/llm_pipeline:/opt/ndnsf-app/python" \
  --env "HOME=/home/${USER}" \
  --env "HF_HOME=/work/huggingface-home" \
  --env "HF_HUB_DISABLE_TELEMETRY=1" \
  --env "TOKENIZERS_PARALLELISM=false" \
  --env "SPEC162_PREP_WORK_ROOT=/work" \
  --env "SPEC162_PREP_SOURCE_ROOT=/source" \
  --env "SPEC162_MODEL_PROFILE=$model_profile" \
  --env "SPEC162_PROMOTED_ARTIFACT_DIR=$artifact_final" \
  --env "SPEC162_PREP_SUBMISSION_ID=$SPEC162_PREP_SUBMISSION_ID" \
  --env "SPEC162_CAPACITY_DECISION_SHA256=$SPEC162_CAPACITY_DECISION_SHA256" \
  --env "SPEC162_SIF_SHA256=$SPEC162_SIF_SHA256" \
  --env "SPEC162_PREP_SOURCE_SHA256=$SPEC162_PREP_SOURCE_SHA256" \
  "$SPEC162_SIF" \
  bash /source/jobs/prepare-stages.sh \
  > "$partial/preparation.log" 2>&1

output="$scratch_prefix/output"
for required in \
  stage-manifest.json reference.json tokenizer-manifest.json \
  policy.yaml model-source-manifest.json smoke-campaign.json
do
  test -f "$output/$required"
done
test "$(find "$output/qwen-transformers-stage-artifacts" \
  -maxdepth 1 -type f -name 'stage-*-qwen-transformers.pt' | wc -l)" -eq 3

python3 - "$SPEC162_CAPACITY_DECISION" "$output" <<'PY'
import json
import os
import sys
decision = json.load(open(sys.argv[1], encoding="utf-8"))
promotion = sum(
    os.path.getsize(os.path.join(root, name))
    for root, _dirs, files in os.walk(sys.argv[2])
    for name in files
)
if promotion > int(decision["durablePromotionBytes"]):
    raise SystemExit(
        f"SPEC162_DURABLE_PROMOTION_ESTIMATE_EXCEEDED:{promotion}>"
        f"{decision['durablePromotionBytes']}")
PY

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
for retained in \
  stage-manifest.json reference.json tokenizer-manifest.json \
  model-source-manifest.json smoke-campaign.json artifact-checksums.sha256
do
  cp "$artifact_partial/$retained" "$partial/$retained"
done
find "$artifact_partial" -type f -exec chmod 400 {} +
find "$artifact_partial" -type d -exec chmod 500 {} +
mv "$artifact_partial" "$artifact_final"

df -B1 "$scratch_prefix" > "$partial/scratch-df-after.txt"
find "$scratch_prefix" -mindepth 1 -delete
rmdir -- "$scratch_prefix"
cleanup_performed=true
terminal_state=PASS
