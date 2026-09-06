#!/usr/bin/env bash
set -euo pipefail

# Repository-owned, explicitly non-qualifying fast path.  It validates one
# frozen SIF, model and one-invocation functional bundle before requesting GPUs.
# This legacy wrapper is intentionally disabled: it accepted ambient values
# and used --export=ALL, which is exactly the drift class the canonical profile
# prevents.  Use submit.sh <gate> <profile> <run-record> instead.
echo "SPEC175_LEGACY_DIAGNOSTIC_WRAPPER_DISABLED: use jobs/spec175/submit.sh with proven-tiger-profile.json and a run record" >&2
exit 2

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)
: "${SIF:?set SIF to the exact staged SIF}"
: "${SIF_SHA256:?set SIF_SHA256 to its sha256}"
: "${MODEL_MANIFEST:?set MODEL_MANIFEST to the pinned model manifest}"
: "${REMOTE_MODEL_ROOT:?set REMOTE_MODEL_ROOT to the external model root}"
: "${SPEC175_BUNDLE:?set SPEC175_BUNDLE to the diagnostic bundle}"

expected=${SIF_SHA256#sha256:}
actual=$(sha256sum "$SIF" | awk '{print $1}')
[[ "$actual" == "$expected" ]] || {
  echo "SPEC175_DIAGNOSTIC_SIF_DIGEST_MISMATCH" >&2
  exit 4
}
[[ -d "$REMOTE_MODEL_ROOT" ]] || {
  echo "SPEC175_DIAGNOSTIC_MODEL_ROOT_MISSING" >&2
  exit 4
}

"$ROOT/packaging/ndnsf-di-container/bin/ndnsf-di-spec175-model-preflight" \
  --manifest "$MODEL_MANIFEST"
"$ROOT/packaging/ndnsf-di-container/bin/ndnsf-di-spec175-functional-preflight" \
  --bundle "$SPEC175_BUNDLE" \
  --gate diagnostic-multi-provider \
  --output "${SPEC175_DIAGNOSTIC_PREFLIGHT_OUTPUT:-$SPEC175_BUNDLE/diagnostic-preflight.json}"

command -v sbatch >/dev/null || { echo "SPEC175_SBATCH_UNAVAILABLE" >&2; exit 4; }
export SPEC175_LOCAL_SIF="$SIF"
export SPEC175_LOCAL_SIF_SHA256="$expected"
export SPEC175_JOB_ROOT="$ROOT/packaging/ndnsf-di-container/jobs/spec175"
export SPEC175_REMOTE_MODEL_ROOT="$REMOTE_MODEL_ROOT"
sbatch_args=(--export=ALL)
if [[ -n "${SPEC175_GPU_GRES:-}" ]]; then
  sbatch_args+=(--gres="$SPEC175_GPU_GRES")
fi
exec sbatch "${sbatch_args[@]}" \
  "$ROOT/packaging/ndnsf-di-container/jobs/spec175/diagnose-multiprovider.sbatch"
