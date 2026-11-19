#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 || "$2" != "--" ]]; then
  echo "usage: $0 OUTPUT_DIR -- RUNTIME_COMMAND [ARG ...]" >&2
  exit 64
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../../.." && pwd)"
output_dir="$1"
shift 2

export PYTHONPATH="$repo_root:$repo_root/NDNSF-DistributedInference${PYTHONPATH:+:$PYTHONPATH}"
require_cuda_args=()
case "${NDNSF_SPEC168_REQUIRE_CUDA:-0}" in
  0) ;;
  1) require_cuda_args=(--require-cuda) ;;
  *) echo "NDNSF_SPEC168_REQUIRE_CUDA must be 0 or 1" >&2; exit 64 ;;
esac
exec python3 "$script_dir/spec168_local_gate.py" \
  --mode real-minindn \
  --output-dir "$output_dir" \
  --hard-timeout-s "${NDNSF_SPEC168_HARD_TIMEOUT_S:-900}" \
  "${require_cuda_args[@]}" \
  -- "$@"
