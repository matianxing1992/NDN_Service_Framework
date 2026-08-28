#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 5 || "$4" != "--" ]]; then
  echo "usage: $0 OUTPUT_DIR SIF_PATH sha256:DIGEST -- INNER_COMMAND [ARG ...]" >&2
  exit 64
fi

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../../.." && pwd)"
output_dir="$1"
sif_path="$2"
expected_digest="$3"
shift 4

if [[ ! -f "$sif_path" ]]; then
  echo "exact SIF is missing: $sif_path" >&2
  exit 66
fi
actual_digest="sha256:$(sha256sum "$sif_path" | awk '{print $1}')"
if [[ "$actual_digest" != "$expected_digest" ]]; then
  echo "exact SIF digest mismatch" >&2
  exit 65
fi
export NDNSF_SPEC168_EXPECTED_SIF_DIGEST="$actual_digest"
require_cuda_args=()
case "${NDNSF_SPEC168_REQUIRE_CUDA:-0}" in
  0) ;;
  1) require_cuda_args=(--require-cuda) ;;
  *) echo "NDNSF_SPEC168_REQUIRE_CUDA must be 0 or 1" >&2; exit 64 ;;
esac

mkdir -p "$output_dir"
output_dir="$(realpath "$output_dir")"
sif_path="$(realpath "$sif_path")"
export PYTHONPATH="$repo_root:$repo_root/NDNSF-DistributedInference${PYTHONPATH:+:$PYTHONPATH}"

exec python3 "$script_dir/spec168_local_gate.py" \
  --mode exact-sif \
  --output-dir "$output_dir" \
  --expected-container-digest "$actual_digest" \
  --hard-timeout-s "${NDNSF_SPEC168_HARD_TIMEOUT_S:-900}" \
  "${require_cuda_args[@]}" \
  -- sudo -n apptainer exec --containall \
       --bind "$repo_root:$repo_root:ro" \
       --bind "$output_dir:$output_dir" \
       --pwd "$repo_root" \
       "$sif_path" "$@"
