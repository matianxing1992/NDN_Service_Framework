#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=3
#SBATCH --ntasks=3
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=32G
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=02:00:00
#SBATCH --signal=B:USR1@120
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec168/tiger-small-repeated/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec168/tiger-small-repeated/slurm-%j.err

set -Eeuo pipefail
umask 077

: "${SPEC168_CAMPAIGN_MANIFEST:?}"
: "${SPEC168_SOURCE_DIR:?}"
: "${SPEC168_SOURCE_BUNDLE_SHA256:?}"
: "${SPEC168_SOURCE_DIGEST:?}"
: "${SPEC168_SIF:?}"
: "${SPEC168_SIF_SHA256:?}"
: "${SPEC168_ARTIFACT_DIR:?}"
: "${SPEC168_ARTIFACT_MANIFEST_SHA256:?}"
: "${SPEC168_GENERATION_TEMPLATE:?}"
: "${SPEC168_REQUEST_ID:?}"

readonly evidence_parent=/project/tma1/ndnsf-di/evidence/spec168/tiger-small-repeated
readonly campaign_id="$(python3 "$SPEC168_SOURCE_DIR/jobs/spec168_campaign.py" \
  validate --manifest "$SPEC168_CAMPAIGN_MANIFEST")"
readonly partial="${evidence_parent}/.${campaign_id}.job-${SLURM_JOB_ID}.partial"
readonly final="${evidence_parent}/${campaign_id}"
readonly failed="${evidence_parent}/${campaign_id}-FAILED-job-${SLURM_JOB_ID}"
test ! -e "$partial"
test ! -e "$final"
test ! -e "$failed"
mkdir -p "$partial"

terminal_state=FAIL
rank_step_pid=
finish()
{
  rc=$?
  trap - EXIT INT TERM USR1
  if test -n "$rank_step_pid"; then
    kill -TERM "$rank_step_pid" 2>/dev/null || true
    wait "$rank_step_pid" 2>/dev/null || true
  fi
  secret_count=0
  if test -d "$partial"; then
    while IFS= read -r secret; do
      case "$secret" in
        "$partial"/*) rm -f -- "$secret"; secret_count=$((secret_count + 1)) ;;
        *) exit 4 ;;
      esac
    done < <(find "$partial" -type f \
      \( -name '*.privkey' -o -name '*.key' -o -name '*.token' \
         -o -name 'bootstrap-tokens.txt' \) -print)
    printf 'removedSecretFileCount=%s\n' "$secret_count" \
      > "$partial/security-scrub.txt" 2>/dev/null || true
  fi
  printf '%s\n' "$terminal_state" > "$partial/terminal-state.txt" 2>/dev/null || true
  printf '%s\n' "$rc" > "$partial/batch-exit-code.txt" 2>/dev/null || true
  if test -d "$partial"; then
    if test "$rc" -eq 0 && test "$terminal_state" = PASS; then
      mv "$partial" "$final"
    else
      mv "$partial" "$failed"
    fi
  fi
  exit "$rc"
}
trap finish EXIT
trap 'exit 130' INT TERM USR1

mkdir -p "$partial/node-0" "$partial/node-1" "$partial/node-2"
printf '%s  %s\n' "${SPEC168_SOURCE_BUNDLE_SHA256#sha256:}" \
  "$SPEC168_SOURCE_DIR/source-files.sha256" | sha256sum -c - \
  > "$partial/source-manifest-sha256.log"
python3 "$SPEC168_SOURCE_DIR/jobs/spec168_verify_source_bundle.py" \
  --root "$SPEC168_SOURCE_DIR" \
  --expected-manifest-digest "$SPEC168_SOURCE_BUNDLE_SHA256" \
  > "$partial/source-files-sha256.log"
printf '%s\n' "$SPEC168_SOURCE_DIGEST" > "$partial/source-identity.digest"
printf '%s\n' "$SPEC168_SOURCE_DIR" > "$partial/source-bundle.path"
printf '%s  %s\n' "${SPEC168_SIF_SHA256#sha256:}" "$SPEC168_SIF" |
  sha256sum -c - > "$partial/sif-sha256.log"
printf '%s  %s\n' "${SPEC168_ARTIFACT_MANIFEST_SHA256#sha256:}" \
  "$SPEC168_ARTIFACT_DIR/stage-manifest.json" | sha256sum -c - \
  > "$partial/stage-manifest-sha256.log"
(cd "$SPEC168_ARTIFACT_DIR" && sha256sum -c artifact-checksums.sha256) \
  > "$partial/artifact-checksums.log"

cp "$SPEC168_CAMPAIGN_MANIFEST" "$partial/campaign-manifest.json"
cp "$SPEC168_ARTIFACT_DIR/stage-manifest.json" "$partial/stage-manifest.json"
cp "$SPEC168_ARTIFACT_DIR/reference.json" "$partial/reference.json"
cp "$SPEC168_SOURCE_DIR/source-files.sha256" "$partial/source-files.sha256"

export SPEC168_PARTIAL="$partial"
python3 "$SPEC168_SOURCE_DIR/jobs/spec168_cold_warm_contract.py" \
  --manifest "$SPEC168_GENERATION_TEMPLATE" \
  > "$partial/formal-campaign-validation.log"
python3 - "$SPEC168_CAMPAIGN_MANIFEST" \
  "$SPEC168_ARTIFACT_DIR/stage-manifest.json" \
  "$SPEC168_GENERATION_TEMPLATE" "$partial/generation-campaign.json" <<'PY'
import hashlib
import json
import os
import sys
from pathlib import Path

campaign_path, stage_path, template_path, output_path = map(Path, sys.argv[1:])
campaign = json.loads(campaign_path.read_text(encoding="utf-8"))
stage = json.loads(stage_path.read_text(encoding="utf-8"))
template = json.loads(template_path.read_text(encoding="utf-8"))
bindings = campaign["bindingDigests"]
if "sha256:" + hashlib.sha256(stage_path.read_bytes()).hexdigest() != \
        bindings["remoteSmallStageManifestDigest"]:
    raise SystemExit("SPEC168_SMALL_STAGE_BINDING_MISMATCH")
request_id = os.environ["SPEC168_REQUEST_ID"]
if not request_id or any(ch not in "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"
                         for ch in request_id):
    raise SystemExit("SPEC168_REQUEST_ID_INVALID")
output_path.write_text(json.dumps(template, indent=2, sort_keys=True) + "\n",
                       encoding="utf-8")
Path(os.environ["SPEC168_PARTIAL"] + "/model-identity.digest").write_text(
    str(stage["modelDigest"]) + "\n", encoding="utf-8")
Path(os.environ["SPEC168_PARTIAL"] + "/workload.digest").write_text(
    str(bindings["promptSetDigest"]) + "\n", encoding="utf-8")
PY

model_identity_digest=$(<"$partial/model-identity.digest")
case "$model_identity_digest" in sha256:*) ;; *) model_identity_digest="sha256:$model_identity_digest" ;; esac
workload_digest=$(<"$partial/workload.digest")
mapfile -t nodes < <(scontrol show hostnames "$SLURM_JOB_NODELIST")
test "${#nodes[@]}" -eq 3
printf '%s\n' "${nodes[@]}" > "$partial/nodes.txt"
port=$((20000 + SLURM_JOB_ID % 20000))
printf '%s\n' "$port" > "$partial/port.txt"

export SPEC168_SHARED="$partial"
export SPEC168_PORT="$port"
export SPEC168_POLICY="$SPEC168_ARTIFACT_DIR/policy.yaml"
export SPEC168_GENERATION_CAMPAIGN="$partial/generation-campaign.json"
export SPEC168_MODEL_IDENTITY_DIGEST="$model_identity_digest"
export SPEC168_WORKLOAD_DIGEST="$workload_digest"
export SPEC168_REQUEST_TIMEOUT_MS="${SPEC168_REQUEST_TIMEOUT_MS:-3600000}"
export SPEC168_ACK_TIMEOUT_MS="${SPEC168_ACK_TIMEOUT_MS:-120000}"
export SPEC168_SELECTION_OFFER_LEASE_MS="${SPEC168_SELECTION_OFFER_LEASE_MS:-3600000}"

srun --kill-on-bad-exit=0 --label \
  --export=ALL \
  bash "$SPEC168_SOURCE_DIR/jobs/spec168-three-node-rank.sh" \
  > "$partial/rank-step.log" 2> "$partial/rank-step.err" &
rank_step_pid=$!
set +e
wait "$rank_step_pid"
rank_step_rc=$?
set -e
rank_step_pid=
test "$rank_step_rc" -eq 0
for rank in 0 1 2; do
  test -f "$partial/rank-complete-${rank}"
  test -f "$partial/node-${rank}/hostname.txt"
  test -f "$partial/node-${rank}/gpu.csv"
  test -f "$partial/node-${rank}/route-list-after-routes.txt"
  test -f "$partial/node-${rank}/face-list-after-routes.txt"
  test -f "$partial/node-${rank}/provider-${rank}.log"
done
test "$(<"$partial/node-0/user-exit.txt")" -eq 0
rm -f -- "$partial/bootstrap-tokens.txt"
test -z "$(find "$partial" -type f \
  \( -name '*.privkey' -o -name '*.key' -o -name '*.token' \) -print -quit)"

python3 "$SPEC168_SOURCE_DIR/jobs/spec168-cold-warm-analyzer.py" \
  --root "$partial" \
  --stage-manifest "$SPEC168_ARTIFACT_DIR/stage-manifest.json" \
  --campaign-manifest "$SPEC168_CAMPAIGN_MANIFEST" \
  --generation-campaign "$partial/generation-campaign.json" \
  --expected-source-digest "$SPEC168_SOURCE_DIGEST" \
  --expected-source-bundle-digest "$SPEC168_SOURCE_BUNDLE_SHA256" \
  --expected-sif-digest "$SPEC168_SIF_SHA256" \
  --request-prefix "$SPEC168_REQUEST_ID" \
  --output-json "$partial/analysis.json" \
  > "$partial/analysis.log" 2>&1
terminal_state=PASS
