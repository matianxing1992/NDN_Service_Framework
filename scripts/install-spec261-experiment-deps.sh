#!/usr/bin/env bash
set -euo pipefail

# Verify (or stage) the global Spec 261 experiment dependencies.
#
# Spec 261 needs two things the product install does not provide: an offline
# Python 3.10 export environment with the pinned Torch/Ultralytics/ONNX stack,
# and the frozen YOLO model assets. Neither belongs inside a checkout: a
# per-checkout `deps/spec261` tree is invisible to the installer, is easy to
# lose and makes runs depend on one developer's working copy. Both live under
# the same global root as every other NDNSF dependency.
#
# Layout:
#   /usr/local/lib/ndnsf/spec261/python        CPython 3.10.22 (uv-managed)
#   /usr/local/lib/ndnsf/spec261/yolo-export   venv with the pinned export stack
#   /usr/local/lib/ndnsf/spec261/yolo-config   Ultralytics settings directory
#   /usr/local/lib/ndnsf/spec261/models/       frozen YOLO ONNX exports
#
# Usage:
#   scripts/install-spec261-experiment-deps.sh [--prefix /usr/local]
#   scripts/install-spec261-experiment-deps.sh --stage-model-source /path/to/canonical
#
# The script never downloads. A missing dependency is reported with the exact
# path it needs; staging a frozen export is a copy from an explicitly supplied
# cache directory.

PREFIX="/usr/local"
STAGE_SOURCE=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --prefix) PREFIX="${2:?--prefix needs a value}"; shift 2;;
    --stage-model-source) STAGE_SOURCE="${2:?--stage-model-source needs a value}"; shift 2;;
    -h|--help) sed -n '2,40p' "$0"; exit 0;;
    *) echo "unknown option: $1" >&2; exit 2;;
  esac
done

ROOT="${PREFIX}/lib/ndnsf/spec261"
VENV="${ROOT}/yolo-export"
PYTHON="${ROOT}/python/cpython-3.10.22-linux-x86_64-gnu/bin/python3.10"
CONFIG="${ROOT}/yolo-config"
MODELS="${ROOT}/models"

fail() { echo "spec261-deps: $*" >&2; exit 1; }

[[ -x "${PYTHON}" ]] || fail "missing pinned CPython: ${PYTHON}"
[[ -x "${VENV}/bin/python" ]] || fail "missing export venv: ${VENV}/bin/python"
[[ -d "${CONFIG}" ]] || fail "missing Ultralytics config directory: ${CONFIG}"

if [[ -n "${STAGE_SOURCE}" ]]; then
  [[ -d "${STAGE_SOURCE}" ]] || fail "model source is not a directory: ${STAGE_SOURCE}"
  mkdir -p "${MODELS}/yolo26n" "${MODELS}/yolo11n" 2>/dev/null || true
  for model in yolo26n yolo11n; do
    for file in "${model}.onnx" "${model}.weights" input.f32 oracle.json; do
      [[ -f "${STAGE_SOURCE}/${file}" ]] || continue
      cp -f "${STAGE_SOURCE}/${file}" "${MODELS}/${model}/${file}"
    done
  done
fi

for model in yolo26n yolo11n; do
  for file in "${model}.onnx" "${model}.weights" input.f32 oracle.json; do
    [[ -f "${MODELS}/${model}/${file}" ]] || fail \
      "missing frozen asset: ${MODELS}/${model}/${file} (pass --stage-model-source)"
  done
done

"${VENV}/bin/python" - <<'PY'
import onnx
import onnxruntime
import torch
import ultralytics
print("spec261-deps: export stack",
      "torch", torch.__version__,
      "ultralytics", ultralytics.__version__,
      "onnx", onnx.__version__,
      "ort", onnxruntime.__version__)
PY

echo "spec261-deps: global root ready at ${ROOT}"
echo "spec261-deps: set SPEC261_ASSET_ROOT=${MODELS} to override the model location"
echo "spec261-deps: set YOLO_CONFIG_DIR=${CONFIG} for Ultralytics settings"
