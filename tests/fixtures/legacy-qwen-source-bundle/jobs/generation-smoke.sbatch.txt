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
#SBATCH --time=16:00:00
#SBATCH --signal=B:USR1@120
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec162/qwen36-smoke/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec162/qwen36-smoke/slurm-%j.err

set -Eeuo pipefail
umask 077

: "${SPEC162_SMOKE_RUN_ID:?}"
: "${SPEC162_SMOKE_SUBMISSION_ID:?}"
: "${SPEC162_SMOKE_SOURCE_ROOT:?}"
: "${SPEC162_SMOKE_SOURCE_SHA256:?}"
: "${SPEC162_SIF:?}"
: "${SPEC162_SIF_SHA256:?}"
: "${SPEC162_ARTIFACT_DIR:?}"
: "${SPEC162_ARTIFACT_MANIFEST_SHA256:?}"

readonly campaign_mode="${SPEC162_CAMPAIGN_MODE:-smoke}"
case "$campaign_mode" in
  smoke|formal) ;;
  *) echo "SPEC162_CAMPAIGN_MODE_INVALID:$campaign_mode" >&2; exit 2 ;;
esac
readonly evidence_model_tag="${SPEC162_EVIDENCE_MODEL_TAG:-qwen36}"
case "$evidence_model_tag" in
  qwen36|qwen3-0.6b) ;;
  *) echo "SPEC162_EVIDENCE_MODEL_TAG_INVALID:$evidence_model_tag" >&2; exit 2 ;;
esac
readonly campaign_name="${campaign_mode}-campaign.json"
readonly evidence_parent="/project/tma1/ndnsf-di/evidence/spec162/${evidence_model_tag}-${campaign_mode}"
readonly partial="${evidence_parent}/.${SPEC162_SMOKE_SUBMISSION_ID}.partial"
readonly final="${evidence_parent}/${SPEC162_SMOKE_SUBMISSION_ID}"
test ! -e "$partial"
test ! -e "$final"
mkdir -p "$partial/source"
printf '%s  %s\n' "$SPEC162_SMOKE_SOURCE_SHA256" \
  "$SPEC162_SMOKE_SOURCE_ROOT/source-files.sha256" |
  sha256sum -c - > "$partial/source-manifest-sha256.log"
(
  cd "$SPEC162_SMOKE_SOURCE_ROOT"
  sha256sum -c source-files.sha256
) > "$partial/source-files-sha256.log"
# The source root and evidence directory normally share the project
# filesystem.  Hard-link the already hash-verified immutable source snapshot
# in that case; this preserves an auditable snapshot without repeatedly
# copying thousands of small files over the slow shared mount.  Fall back to
# a byte copy when the roots are on different filesystems.
source_snapshot_mode=copy
if test "$(stat -c '%d' "$SPEC162_SMOKE_SOURCE_ROOT")" = \
       "$(stat -c '%d' "$partial")"; then
  cp -al "$SPEC162_SMOKE_SOURCE_ROOT"/jobs "$partial/source/"
  cp -al "$SPEC162_SMOKE_SOURCE_ROOT"/llm_pipeline "$partial/source/"
  cp -al "$SPEC162_SMOKE_SOURCE_ROOT"/runtime "$partial/source/"
  source_snapshot_mode=hardlink
else
  cp -a "$SPEC162_SMOKE_SOURCE_ROOT"/jobs "$partial/source/"
  cp -a "$SPEC162_SMOKE_SOURCE_ROOT"/llm_pipeline "$partial/source/"
  cp -a "$SPEC162_SMOKE_SOURCE_ROOT"/runtime "$partial/source/"
fi
printf 'mode=%s\n' "$source_snapshot_mode" > "$partial/source-snapshot-mode.log"
cp "$SPEC162_SMOKE_SOURCE_ROOT/source-files.sha256" \
  "$partial/source/source-files.sha256"
printf '%s  source-bundle\n' "$SPEC162_SMOKE_SOURCE_SHA256" \
  > "$partial/source-bundle.sha256"
printf '%s  %s\n' "$SPEC162_SIF_SHA256" "$SPEC162_SIF" |
  sha256sum -c - > "$partial/sif-sha256.log"
printf '%s  %s\n' "$SPEC162_ARTIFACT_MANIFEST_SHA256" \
  "$SPEC162_ARTIFACT_DIR/stage-manifest.json" |
  sha256sum -c - > "$partial/stage-manifest-sha256.log"
(
  cd "$SPEC162_ARTIFACT_DIR"
  sha256sum -c artifact-checksums.sha256
) > "$partial/artifact-checksums.log"
# The stage manifest is an immutable artifact contract, but its paths are
# deployment-local.  Reject a manifest that still names a previous promoted
# artifact before any provider is allocated or a request is sent.  The stage
# digest checks above cannot catch this class of stale-path rebinding error.
python3 - "$SPEC162_ARTIFACT_DIR/stage-manifest.json" \
  "$SPEC162_ARTIFACT_DIR" <<'PY'
import json
import sys
from pathlib import Path

manifest_path, artifact_dir = map(Path, sys.argv[1:])
artifact_dir = artifact_dir.resolve()
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
runtime = str((manifest.get("runtime") or {}).get("backend", ""))
stage_dir = (
    "qwen-onnx-stage-artifacts"
    if runtime == "onnxruntime"
    else "qwen-transformers-stage-artifacts"
)
stage_root = (artifact_dir / stage_dir).resolve()
stages = manifest.get("stages")
if not isinstance(stages, list) or not stages:
    raise SystemExit("SPEC162_STAGE_PATH_MISMATCH:missing-stages")
for index, stage in enumerate(stages):
    filename = str(stage.get("filename", ""))
    if not filename or Path(filename).name != filename:
        raise SystemExit(
            f"SPEC162_STAGE_PATH_MISMATCH:stage={index}:invalid-filename")
    expected = (stage_root / filename).resolve()
    declared = Path(str(stage.get("path", ""))).expanduser()
    if declared.resolve(strict=False) != expected:
        raise SystemExit(
            f"SPEC162_STAGE_PATH_MISMATCH:stage={index}:"
            f"declared={declared}:expected={expected}")
    if not expected.is_file():
        raise SystemExit(
            f"SPEC162_STAGE_PATH_MISSING:stage={index}:path={expected}")
PY
# The provider builds its service policy before the first ACK.  Its sealed
# Qwen artifact paths must bind to the same current stage manifest; otherwise
# an old artifact can remain silently usable on the shared filesystem and
# invalidate the source/artifact identity even though the stage digests match.
python3 - "$SPEC162_ARTIFACT_DIR/stage-manifest.json" \
  "$SPEC162_ARTIFACT_DIR/policy.yaml" <<'PY'
import json
import sys
from pathlib import Path

import yaml

manifest_path, policy_path = map(Path, sys.argv[1:])
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
expected = {
    str(stage["role"]): str(Path(stage["path"]).resolve())
    for stage in manifest.get("stages", ())
}
expected_runtime = str((manifest.get("runtime") or {}).get("backend", ""))
if len(expected) != 3:
    raise SystemExit("SPEC162_POLICY_STAGE_PATH_MISMATCH:manifest-coverage")
policy = yaml.safe_load(policy_path.read_text(encoding="utf-8")) or {}
observed = {}
for service in policy.get("services", ()):
    for artifact in service.get("artifacts", ()):
        if artifact.get("kind") not in {"llm-stage-weights", "onnx-model"}:
            continue
        role = str(artifact.get("role", ""))
        if role in expected:
            observed_runtime = str((artifact.get("metadata") or {}).get("runtime", ""))
            if expected_runtime == "onnxruntime" and (
                    artifact.get("kind") != "onnx-model"
                    or observed_runtime != "onnxruntime"):
                raise SystemExit(
                    "SPEC162_POLICY_STAGE_RUNTIME_MISMATCH:"
                    f"role={role}:expected=onnxruntime")
            observed[role] = str(Path(str(artifact.get("path", ""))).resolve())
if set(observed) != set(expected):
    raise SystemExit(
        "SPEC162_POLICY_STAGE_PATH_MISMATCH:role-coverage:"
        f"expected={sorted(expected)}:observed={sorted(observed)}")
for role, path in expected.items():
    if observed[role] != path:
        raise SystemExit(
            "SPEC162_POLICY_STAGE_PATH_MISMATCH:"
            f"role={role}:declared={observed[role]}:expected={path}")
PY
cp "$SPEC162_ARTIFACT_DIR/stage-manifest.json" "$partial/stage-manifest.json"
cp "$SPEC162_ARTIFACT_DIR/reference.json" "$partial/reference.json"
# A fixed Provider/User settle is a historical diagnostic mode, not the
# NDNSF-DI request lifecycle. Reject an accidentally reused old source bundle
# before allocating three GPUs or starting any runtime process.
python3 - "$SPEC162_SMOKE_SOURCE_ROOT/jobs/generation-rank.sh" \
  "$SPEC162_SMOKE_SOURCE_ROOT/jobs/generation-rank-inner.sh" <<'PY'
import re
import sys

for path in sys.argv[1:]:
    text = open(path, encoding="utf-8").read()
    forbidden = (
        "SPEC162_PROVIDER_SETTLE_SECONDS",
        "SPEC162_USER_STARTUP_SETTLE_MS",
        "provider_settle_seconds",
        "user_startup_settle_ms",
    )
    for marker in forbidden:
        if marker in text:
            raise SystemExit(
                f"SPEC162_FIXED_SETTLE_FORBIDDEN:{path}:{marker}")
    if re.search(r"(^|[;&|])\s*sleep\s+300(?:\.0+)?\s*(?:$|[;&|])",
                 text, re.MULTILINE):
        raise SystemExit(f"SPEC162_FIXED_SETTLE_FORBIDDEN:{path}:sleep-300")
PY
python3 - "$partial/stage-manifest.json" "$SPEC162_SIF_SHA256" <<'PY'
import json
import sys

manifest_path, runtime_sif_sha256 = sys.argv[1:]
manifest = json.loads(open(manifest_path, encoding="utf-8").read())
def bare(value):
    value = str(value)
    return value[7:] if value.startswith("sha256:") else value
expected = bare(manifest.get("runtimeSifSha256", ""))
actual = bare(runtime_sif_sha256)
if not expected:
    raise SystemExit("SPEC162_STAGE_MANIFEST_RUNTIME_SIF_MISSING")
if expected != actual:
    raise SystemExit(
        "SPEC162_STAGE_MANIFEST_RUNTIME_SIF_MISMATCH:"
        f"expected={expected}:actual={actual}")
PY
python3 - "$SPEC162_ARTIFACT_DIR/$campaign_name" \
  "$partial/$campaign_name" <<'PY'
import json
import os
import sys

source, target = sys.argv[1:]
with open(source, encoding="utf-8") as stream:
    campaign = json.load(stream)
generation = dict(campaign.get("generation", {}))
if int(generation.get("maxNewTokens", 0)) != 64:
    raise SystemExit("SPEC162_CAMPAIGN_MAX_NEW_TOKENS_MISMATCH")
generation["requireEos"] = False
campaign["generation"] = generation
submission_id = os.environ.get("SPEC162_SMOKE_SUBMISSION_ID", "").strip()
if not submission_id:
    raise SystemExit("SPEC162_SMOKE_SUBMISSION_ID_MISSING_FOR_CAMPAIGN")
# The artifact's campaign is a reusable template.  Bind the copied manifest
# to this immutable submission so every token request carries the same fresh
# source identity and cannot collide with an earlier Tiger run.
campaign_mode = os.environ.get("SPEC162_CAMPAIGN_MODE", "smoke").strip()
campaign["campaignId"] = f"{submission_id}-{campaign_mode}-max64"
with open(target, "w", encoding="utf-8") as stream:
    json.dump(campaign, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY

terminal_state=FAIL
rank_step_pid=
cancel_requested=0
request_cancel()
{
  terminal_state=CANCELLED
  cancel_requested=1
  if test -n "$rank_step_pid"; then
    kill -TERM "$rank_step_pid" 2>/dev/null || true
  fi
}
finish()
{
  rc=$?
  trap - EXIT INT TERM USR1
  rm -f -- "$partial/bootstrap-tokens.txt"
  rm -f -- "$partial/selection-offer-0.key" \
    "$partial/selection-offer-1.key" \
    "$partial/selection-offer-2.key" \
    "$partial/selection-offer-key-map.json"
  python3 - "$partial/result.json" "$terminal_state" "$rc" <<'PY' || true
import json
import os
import sys

path, state, return_code = sys.argv[1:]
with open(path, "w", encoding="utf-8") as stream:
    campaign_mode = os.environ.get("SPEC162_CAMPAIGN_MODE", "smoke")
    json.dump({
        "schemaVersion":
            f"ndnsf-di-qwen36-generation-{campaign_mode}-result-v1",
        "campaignMode": campaign_mode,
        "evidenceModelTag":
            os.environ.get("SPEC162_EVIDENCE_MODEL_TAG", "qwen36"),
        "state": state,
        "exitCode": int(return_code),
        "jobId": os.environ["SLURM_JOB_ID"],
        "runId": os.environ["SPEC162_SMOKE_RUN_ID"],
        "submissionId": os.environ["SPEC162_SMOKE_SUBMISSION_ID"],
        "sourceSha256": os.environ["SPEC162_SMOKE_SOURCE_SHA256"],
        "sifSha256": os.environ["SPEC162_SIF_SHA256"],
        "stageManifestSha256":
            os.environ["SPEC162_ARTIFACT_MANIFEST_SHA256"],
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
trap finish EXIT
trap request_cancel INT TERM USR1

comment=$(scontrol show job "$SLURM_JOB_ID" -o |
  tr ' ' '\n' | sed -n 's/^Comment=//p')
test "$comment" = "spec162:${SPEC162_SMOKE_SUBMISSION_ID}"
mapfile -t nodes < <(scontrol show hostnames "$SLURM_JOB_NODELIST")
printf '%s\n' "${nodes[@]}" > "$partial/nodes.txt"
test "${#nodes[@]}" -eq 3
port=$((20000 + SLURM_JOB_ID % 20000))
printf '%s\n' "$port" > "$partial/port.txt"
export SPEC162_SHARED="$partial"
export SPEC162_PORT="$port"
export SPEC162_POLICY="$SPEC162_ARTIFACT_DIR/policy.yaml"
export SPEC162_SMOKE_CAMPAIGN="$partial/$campaign_name"
srun --kill-on-bad-exit=0 --label \
  --export=ALL,SPEC162_SHARED="$partial",SPEC162_SIF="$SPEC162_SIF",SPEC162_PORT="$port",SPEC162_ARTIFACT_DIR="$SPEC162_ARTIFACT_DIR",SPEC162_POLICY="$SPEC162_POLICY",SPEC162_SMOKE_CAMPAIGN="$SPEC162_SMOKE_CAMPAIGN",SPEC162_SMOKE_SUBMISSION_ID="$SPEC162_SMOKE_SUBMISSION_ID",SPEC162_CAMPAIGN_MODE="$campaign_mode",SPEC162_FACE_SCHEME="${SPEC162_FACE_SCHEME:-tcp4}" \
  bash "$partial/source/jobs/generation-rank.sh" \
  > "$partial/rank-step.log" 2> "$partial/rank-step.err" &
rank_step_pid=$!
set +e
wait "$rank_step_pid"
rank_step_rc=$?
set -e
rank_step_pid=
if test "$cancel_requested" -eq 1; then
  exit 130
fi
test "$rank_step_rc" -eq 0
test -f "$partial/rank-complete-0"
test -f "$partial/rank-complete-1"
test -f "$partial/rank-complete-2"
test -f "$partial/node-0/generation-raw.jsonl"
test -f "$partial/repo-registration.json"
test -f "$partial/automatic-planning.json"
test "$(<"$partial/node-0/user-exit.txt")" = 0
# Search only captured runtime output.  The source bundle intentionally
# contains the literal fail-closed CPU markers and ONNX provider names as
# implementation/tests; scanning ``$partial`` would therefore reject a
# successful run merely because its sealed source is present in evidence.
if grep -RniE \
  'double free|invalid pointer|munmap_chunk|Aborted|core dumped|CPUExecutionProvider|QWEN_STAGE_CPU_FALLBACK_FORBIDDEN|QWEN_STAGE_CUDA_UNAVAILABLE|reportOperationStatus.*incompatible function arguments' \
  --exclude='*ort-profile*' \
  "$partial"/node-* "$partial"/rank-step.log "$partial"/rank-step.err; then
  echo SPEC162_SMOKE_FORBIDDEN_RUNTIME_MARKER >&2
  exit 8
fi
# The controller's bootstrap file is an ephemeral shared credential used only
# while the three ranks start.  Remove it before the post-run secret scan;
# otherwise every otherwise-successful request run fails before analysis.
rm -f -- "$partial/bootstrap-tokens.txt"
test ! -e "$partial/bootstrap-tokens.txt"
test -z "$(find "$partial" -type f \
  \( -name '*.privkey' -o -name '*.key' -o -name '*.token' \
     -o -name 'bootstrap-tokens.txt' \) -print -quit)"
python3 - "$partial/automatic-planning.json" \
  "$partial/repo-registration.json" <<'PY'
import json
import sys
from pathlib import Path

planning = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
registration = json.loads(Path(sys.argv[2]).read_text(encoding="utf-8"))
assert planning["preSplitCatalog"]["publicationState"] == (
    "REQUIRES_DISTRIBUTED_REPO_REGISTRATION")
assert registration["schemaVersion"] == "ndnsf-di-qwen36-repo-registration-v1"
assert len(registration["artifacts"]) == 3
assert {
    (item["role"], item["sha256"], item["bytes"])
    for item in planning["stages"]
} == {
    (item["role"], item["fileSha256"], item["fileBytes"])
    for item in registration["artifacts"]
}
PY
if test "$campaign_mode" = smoke; then
  analyzer="$partial/source/jobs/analyze-generation-smoke.py"
  if python3 - "$partial/node-0/generation-raw.jsonl" <<'PY'
import json
import sys
rows = [json.loads(line) for line in open(sys.argv[1], encoding="utf-8") if line.strip()]
steps = rows[0].get("tokenSteps", []) if len(rows) == 1 else []
raise SystemExit(0 if len(steps) == 1 and steps[0].get("mode") == "FULL" else 1)
PY
  then
    analyzer="$partial/source/jobs/analyze-generation-full-smoke.py"
  fi
  python3 "$analyzer" \
    --root "$partial" \
    --stage-manifest "$SPEC162_ARTIFACT_DIR/stage-manifest.json" \
    --output-json "$partial/analysis.json" \
    --enriched-jsonl "$partial/generation-enriched.jsonl" \
    > "$partial/analysis.log" 2>&1
else
  python3 "$partial/source/jobs/analyze-generation-formal.py" \
    --root "$partial" \
    --campaign "$SPEC162_SMOKE_CAMPAIGN" \
    --output-json "$partial/analysis.json" \
    > "$partial/analysis.log" 2>&1
fi
terminal_state=PASS
