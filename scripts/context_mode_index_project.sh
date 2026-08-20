#!/usr/bin/env bash
set -Eeuo pipefail

# Refresh the stable project Context Mode layer.  This is deliberately
# independent of .specify/feature.json: switching the active Spec must not
# replace the project ContentDB identity or erase cross-Spec material.
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
context_mode_bin="${CONTEXT_MODE_BIN:-context-mode}"
project_root="${CONTEXT_MODE_PROJECT_ROOT:-$root}"

index() {
  local path=$1
  local source=$2
  "$context_mode_bin" index "$path" \
    --project "$project_root" \
    --source "$source"
}

index "$project_root/.specify/memory/context-mode-project.md" \
  "NDNSF project context anchor"
index "$project_root/.specify/memory/context-mode.md" \
  "NDNSF Context Mode operating guide"
if [[ -f "$project_root/.specify/memory/constitution.md" ]]; then
  index "$project_root/.specify/memory/constitution.md" \
    "NDNSF project constitution"
fi

# Historical and active Specs share the project layer.  Only maintained
# Markdown/JSON documents are eligible; source trees, results, logs, and build
# output remain outside Context Mode and are handled by CodeGraph/analyzers.
if [[ -d "$project_root/specs" ]]; then
  for feature in "$project_root"/specs/*; do
    [[ -d "$feature" ]] || continue
    feature_name=$(basename "$feature")
    for filename in spec.md plan.md tasks.md data-model.md quickstart.md; do
      path="$feature/$filename"
      [[ -f "$path" ]] || continue
      index "$path" "NDNSF Spec ${feature_name}:$path"
    done
  done
fi

# Keep the mutable active pointer and the strict active-Spec source set in the
# same client store, then run the default project-layer health check.
export CONTEXT_MODE_BIN="$context_mode_bin"
export CONTEXT_MODE_PROJECT_ROOT="$project_root"
bash "$project_root/scripts/context_mode_index_authority.sh"
