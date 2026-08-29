#!/usr/bin/env bash
set -euo pipefail

# One frozen submission boundary for Spec175. This only submits an already
# qualified local SIF; it never builds, materializes, or overlays a runtime.
ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
GATE=${1:-}
case "$GATE" in
  control) JOB="$ROOT/packaging/ndnsf-di-container/jobs/spec175/qualify-control.sbatch"; CHECKLIST_GATE=control ;;
  stage-readiness) JOB="$ROOT/packaging/ndnsf-di-container/jobs/spec175/qualify-stage-readiness.sbatch"; CHECKLIST_GATE=stage-readiness ;;
  multi-provider) JOB="$ROOT/packaging/ndnsf-di-container/jobs/spec175/qualify-multiprovider.sbatch"; CHECKLIST_GATE=functional ;;
  conversation-residency) JOB="$ROOT/packaging/ndnsf-di-container/jobs/spec175/qualify-conversation-residency.sbatch"; CHECKLIST_GATE=functional ;;
  performance) JOB="$ROOT/packaging/ndnsf-di-container/jobs/spec175/qualify-performance.sbatch"; CHECKLIST_GATE=performance ;;
  *) echo "usage: $0 {control|stage-readiness|multi-provider|conversation-residency|performance}" >&2; exit 2 ;;
esac

: "${CLOSURE_MANIFEST:?set CLOSURE_MANIFEST to the PASS G0-G4 closure manifest}"
: "${SIF:?set SIF to the exact local candidate SIF}"
: "${SIF_SHA256:?set SIF_SHA256 to the candidate sha256}"
: "${WORKLOAD:?set WORKLOAD to the frozen workload.json}"
: "${REMOTE_SIF:?set REMOTE_SIF to the immutable remote SIF path}"
: "${REMOTE_SIF_SHA256:?set REMOTE_SIF_SHA256 to the expected remote SIF sha256}"

if [[ "$CHECKLIST_GATE" != control ]]; then
  : "${MODEL_MANIFEST:?set MODEL_MANIFEST to the external content-addressed model manifest}"
  : "${REMOTE_MODEL_ROOT:?set REMOTE_MODEL_ROOT to the external model root}"
  MODEL_PREFLIGHT="$ROOT/packaging/ndnsf-di-container/bin/ndnsf-di-spec175-model-preflight"
  [[ -x "$MODEL_PREFLIGHT" ]] || { echo "SPEC175_MODEL_PREFLIGHT_MISSING" >&2; exit 4; }
  "$MODEL_PREFLIGHT" --manifest "$MODEL_MANIFEST"
fi

PRE_TIGER_CHECKLIST="${PRE_TIGER_CHECKLIST:-${SPEC175_PRE_TIGER_CHECKLIST:-}}"
: "${PRE_TIGER_CHECKLIST:?set PRE_TIGER_CHECKLIST (or SPEC175_PRE_TIGER_CHECKLIST) to the candidate-bound checklist}"
PRE_TIGER_CHECKLIST_VALIDATION="${PRE_TIGER_CHECKLIST_VALIDATION:-${SPEC175_PRE_TIGER_CHECKLIST_VALIDATION:-$(dirname "$PRE_TIGER_CHECKLIST")/pre-tiger-checklist-validation.json}}"
CHECKLIST_VALIDATOR="$ROOT/packaging/ndnsf-di-container/bin/ndnsf-di-pre-tiger-checklist"
[[ -x "$CHECKLIST_VALIDATOR" ]] || { echo "SPEC175_CHECKLIST_VALIDATOR_MISSING" >&2; exit 4; }

# A control bundle is intentionally insufficient for G6/G6C/G7.  Validate the
# functional command bundle before any candidate-manifest processing that can
# lead to SSH, upload, or sbatch.  This catches the historical failure mode in
# which the multi-provider selector launched the HELLO-only control workload.
if [[ "$CHECKLIST_GATE" == functional ]]; then
  : "${SPEC175_BUNDLE:?set SPEC175_BUNDLE to the staged functional bundle}"
  FUNCTIONAL_PREFLIGHT="$ROOT/packaging/ndnsf-di-container/bin/ndnsf-di-spec175-functional-preflight"
  [[ -x "$FUNCTIONAL_PREFLIGHT" ]] || {
    echo "SPEC175_FUNCTIONAL_PREFLIGHT_MISSING" >&2
    exit 4
  }
  FUNCTIONAL_PREFLIGHT_OUTPUT="${SPEC175_FUNCTIONAL_PREFLIGHT_OUTPUT:-$(dirname "$PRE_TIGER_CHECKLIST")/functional-bundle-preflight.json}"
  "$FUNCTIONAL_PREFLIGHT" \
    --bundle "$SPEC175_BUNDLE" \
    --gate "$GATE" \
    --output "$FUNCTIONAL_PREFLIGHT_OUTPUT"
fi

python3 - "$CLOSURE_MANIFEST" "$SIF" "$SIF_SHA256" "$WORKLOAD" "${MODEL_MANIFEST:-}" "$CHECKLIST_GATE" <<'PY'
import hashlib
import json
import pathlib
import sys

closure = pathlib.Path(sys.argv[1])
sif = pathlib.Path(sys.argv[2])
expected = sys.argv[3]
workload = pathlib.Path(sys.argv[4])
model = pathlib.Path(sys.argv[5]) if sys.argv[5] else None
gate = sys.argv[6]
doc = json.loads(closure.read_text())
if doc.get("status") not in {"PASS", "PROMOTABLE"}:
    raise SystemExit("SPEC175_GATE_CLOSURE_NOT_PASS")
candidate = doc.get("candidate", {})
if candidate.get("sifPath") not in {None, str(sif.resolve()), str(sif)}:
    raise SystemExit("SPEC175_CANDIDATE_SIF_MISMATCH")

def digest(path):
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(chunk)
    return value.hexdigest()

expected_digest = expected[7:] if expected.startswith("sha256:") else expected
if expected_digest != digest(sif):
    raise SystemExit("SPEC175_LOCAL_SIF_DIGEST_MISMATCH")
if not workload.is_file() or (gate != "control" and (model is None or not model.is_file())):
    raise SystemExit("SPEC175_SUBMISSION_INPUT_MISSING")
print(json.dumps({"status": "PASS", "sifSha256": "sha256:" + digest(sif),
                  "workload": str(workload.resolve()),
                  "modelManifest": None if model is None else str(model.resolve())}))
PY

# This is deliberately before command discovery, SSH/upload, remote mutation,
# or sbatch.  The validator also binds the checklist to this exact closure,
# positional gate, local SIF path and digest.
"$CHECKLIST_VALIDATOR" \
  --manifest "$PRE_TIGER_CHECKLIST" \
  --gate "$CHECKLIST_GATE" \
  --output "$PRE_TIGER_CHECKLIST_VALIDATION" \
  --candidate-manifest "$CLOSURE_MANIFEST" \
  --expected-sif "$SIF" \
  --expected-sif-sha256 "$SIF_SHA256"

command -v sbatch >/dev/null || { echo "SPEC175_SBATCH_UNAVAILABLE" >&2; exit 4; }
export SPEC175_LOCAL_SIF="$SIF"
export SPEC175_LOCAL_SIF_SHA256="${SIF_SHA256#sha256:}"
export SPEC175_JOB_ROOT="$ROOT/packaging/ndnsf-di-container/jobs/spec175"
export SPEC175_REMOTE_SIF="$REMOTE_SIF"
export SPEC175_REMOTE_SIF_SHA256="${REMOTE_SIF_SHA256#sha256:}"
export SPEC175_WORKLOAD="$WORKLOAD"
if [[ "$CHECKLIST_GATE" != control ]]; then
  export SPEC175_MODEL_MANIFEST="$MODEL_MANIFEST"
  export SPEC175_REMOTE_MODEL_ROOT
fi
exec sbatch --export=ALL "$JOB"
