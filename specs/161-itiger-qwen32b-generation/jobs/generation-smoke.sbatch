#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=3
#SBATCH --ntasks=3
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=96G
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=01:00:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec161/qwen32b-smoke/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec161/qwen32b-smoke/slurm-%j.err

set -Eeuo pipefail
umask 077

: "${SPEC161_SMOKE_RUN_ID:?}"
: "${SPEC161_SMOKE_SUBMISSION_ID:?}"
: "${SPEC161_SMOKE_SOURCE_ROOT:?}"
: "${SPEC161_SMOKE_SOURCE_SHA256:?}"
: "${SPEC161_SIF:?}"
: "${SPEC161_SIF_SHA256:?}"
: "${SPEC161_ARTIFACT_DIR:?}"
: "${SPEC161_ARTIFACT_MANIFEST_SHA256:?}"

readonly evidence_parent=/project/tma1/ndnsf-di/evidence/spec161/qwen32b-smoke
readonly partial="${evidence_parent}/.${SPEC161_SMOKE_SUBMISSION_ID}.partial"
readonly final="${evidence_parent}/${SPEC161_SMOKE_SUBMISSION_ID}"
test ! -e "$partial"
test ! -e "$final"
mkdir -p "$partial/source"
cp -a "$SPEC161_SMOKE_SOURCE_ROOT"/jobs "$partial/source/"
cp -a "$SPEC161_SMOKE_SOURCE_ROOT"/llm_pipeline "$partial/source/"
printf '%s  source-bundle\n' "$SPEC161_SMOKE_SOURCE_SHA256" \
  > "$partial/source-bundle.sha256"
printf '%s  %s\n' "$SPEC161_SIF_SHA256" "$SPEC161_SIF" |
  sha256sum -c - > "$partial/sif-sha256.log"
printf '%s  %s\n' "$SPEC161_ARTIFACT_MANIFEST_SHA256" \
  "$SPEC161_ARTIFACT_DIR/stage-manifest.json" |
  sha256sum -c - > "$partial/stage-manifest-sha256.log"
(
  cd "$SPEC161_ARTIFACT_DIR"
  sha256sum -c artifact-checksums.sha256
) > "$partial/artifact-checksums.log"
cp "$SPEC161_ARTIFACT_DIR/stage-manifest.json" "$partial/stage-manifest.json"
cp "$SPEC161_ARTIFACT_DIR/reference.json" "$partial/reference.json"
cp "$SPEC161_ARTIFACT_DIR/smoke-campaign.json" "$partial/smoke-campaign.json"

terminal_state=FAIL
finish()
{
  rc=$?
  trap - EXIT INT TERM
  rm -f -- "$partial/bootstrap-tokens.txt"
  python3 - "$partial/result.json" "$terminal_state" "$rc" <<'PY' || true
import json
import os
import sys

path, state, return_code = sys.argv[1:]
with open(path, "w", encoding="utf-8") as stream:
    json.dump({
        "schemaVersion": "ndnsf-di-qwen32b-generation-smoke-result-v1",
        "state": state,
        "exitCode": int(return_code),
        "jobId": os.environ["SLURM_JOB_ID"],
        "runId": os.environ["SPEC161_SMOKE_RUN_ID"],
        "submissionId": os.environ["SPEC161_SMOKE_SUBMISSION_ID"],
        "sourceSha256": os.environ["SPEC161_SMOKE_SOURCE_SHA256"],
        "sifSha256": os.environ["SPEC161_SIF_SHA256"],
        "stageManifestSha256":
            os.environ["SPEC161_ARTIFACT_MANIFEST_SHA256"],
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
test "$comment" = "spec161:${SPEC161_SMOKE_SUBMISSION_ID}"
mapfile -t nodes < <(scontrol show hostnames "$SLURM_JOB_NODELIST")
printf '%s\n' "${nodes[@]}" > "$partial/nodes.txt"
test "${#nodes[@]}" -eq 3
port=$((20000 + SLURM_JOB_ID % 20000))
printf '%s\n' "$port" > "$partial/port.txt"
export SPEC161_SHARED="$partial"
export SPEC161_PORT="$port"
export SPEC161_POLICY="$SPEC161_ARTIFACT_DIR/policy.yaml"
export SPEC161_SMOKE_CAMPAIGN="$SPEC161_ARTIFACT_DIR/smoke-campaign.json"
srun --kill-on-bad-exit=0 --label \
  --export=ALL,SPEC161_SHARED="$partial",SPEC161_SIF="$SPEC161_SIF",SPEC161_PORT="$port",SPEC161_ARTIFACT_DIR="$SPEC161_ARTIFACT_DIR",SPEC161_POLICY="$SPEC161_POLICY",SPEC161_SMOKE_CAMPAIGN="$SPEC161_SMOKE_CAMPAIGN" \
  "$partial/source/jobs/generation-rank.sh" \
  > "$partial/rank-step.log" 2> "$partial/rank-step.err"
test -f "$partial/rank-complete-0"
test -f "$partial/rank-complete-1"
test -f "$partial/rank-complete-2"
test -f "$partial/node-0/generation-raw.jsonl"
test "$(<"$partial/node-0/user-exit.txt")" = 0
if grep -RniE \
  'double free|invalid pointer|munmap_chunk|Aborted|core dumped|CPUExecutionProvider|QWEN_STAGE_CPU_FALLBACK_FORBIDDEN|QWEN_STAGE_CUDA_UNAVAILABLE|reportOperationStatus.*incompatible function arguments' \
  "$partial"/node-* "$partial"/rank-step.log "$partial"/rank-step.err; then
  echo SPEC161_SMOKE_FORBIDDEN_RUNTIME_MARKER >&2
  exit 8
fi
test ! -e "$partial/bootstrap-tokens.txt"
test -z "$(find "$partial" -type f \
  \( -name '*.privkey' -o -name 'bootstrap-tokens.txt' \) -print -quit)"
python3 "$partial/source/jobs/analyze-generation-smoke.py" \
  --root "$partial" \
  --stage-manifest "$SPEC161_ARTIFACT_DIR/stage-manifest.json" \
  --output-json "$partial/analysis.json" \
  --enriched-jsonl "$partial/generation-enriched.jsonl" \
  > "$partial/analysis.log" 2>&1
terminal_state=PASS
