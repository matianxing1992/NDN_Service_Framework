#!/usr/bin/env bash
set -euo pipefail

printf 'NDNSF_DISCOVERY|USER|%s\n' "$USER"
printf 'NDNSF_DISCOVERY|HOST|%s\n' "$(hostname -s)"
for target in "/project/$USER" /tmp; do
  read -r total used available _ < <(df -B1 --output=size,used,avail,target "$target" | tail -1)
  printf 'NDNSF_DISCOVERY|DF|%s|%s|%s|%s\n' "$target" "$total" "$used" "$available"
done

# Preserve administrator guidance as non-command-verified unless a supported
# quota command emits an unambiguous byte record in a future adapter revision.
printf 'NDNSF_DISCOVERY|QUOTA|cluster-admin-guidance|0|200000000000|false\n'
if command -v lfs >/dev/null 2>&1; then
  printf 'NDNSF_DISCOVERY_RAW_QUOTA_BEGIN\n'
  lfs quota -u "$USER" /project 2>&1 || true
  printf 'NDNSF_DISCOVERY_RAW_QUOTA_END\n'
fi
sinfo -h -o '%P|%N|%G|%T' | while IFS='|' read -r partition nodes gres state; do
  printf 'NDNSF_DISCOVERY|GRES|%s|%s|%s|%s\n' "$partition" "$nodes" "$gres" "$state"
done
printf 'NDNSF_DISCOVERY|APPTAINER|%s\n' "$(apptainer version 2>&1 | head -1)"
if curl -fsSI --max-time 10 https://huggingface.co/ >/dev/null 2>&1; then
  printf 'NDNSF_DISCOVERY|EGRESS|PASS\n'
else
  printf 'NDNSF_DISCOVERY|EGRESS|FAIL\n'
fi
