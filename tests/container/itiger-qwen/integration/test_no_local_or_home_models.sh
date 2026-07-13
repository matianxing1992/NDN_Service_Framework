#!/usr/bin/env bash
set -euo pipefail
repo="$(cd "$(dirname "$0")/../../../.." && pwd)"
stage="$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/stage-qwen-model.py"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/source"; printf '{}\n' >"$tmp/source/config.json"
scan_models() { { find "$HOME" -xdev -type f \( -name '*.safetensors' -o -name '*.bin' \) -printf '%p\n' 2>/dev/null || true; } | sort | sha256sum; }
before="$(scan_models)"
if "$stage" --repository Qwen/Qwen2.5-0.5B-Instruct --revision aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa --destination "$HOME/ndnsf-di-model-test" --manifest "$tmp/manifest.json" --license-class apache-2.0 --source-dir "$tmp/source" >"$tmp/out" 2>"$tmp/err"; then
  echo 'expected /home destination rejection' >&2; exit 1
fi
grep -q TRANSFER_DESTINATION_INVALID "$tmp/err"
after="$(scan_models)"
test "$before" = "$after"
test ! -e "$HOME/ndnsf-di-model-test"
printf 'SPEC109_NO_LOCAL_OR_HOME_MODELS_PASS\n'
