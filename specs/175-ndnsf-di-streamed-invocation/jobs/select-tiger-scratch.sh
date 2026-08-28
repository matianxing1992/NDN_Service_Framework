#!/usr/bin/env bash
# Select a writable node-local scratch directory with bounded probes.
# Sourced by a Slurm job; exports scratch_root, scratch, and free_bytes.
set -Eeuo pipefail

readonly scratch_candidates=(
  "${SLURM_TMPDIR:-}"
  "${TMPDIR:-}"
  "/tmp"
  "/scratch"
)
scratch_root=""
scratch=""
free_bytes=""
for candidate in "${scratch_candidates[@]}"; do
  test -n "$candidate" || continue
  candidate="${candidate%/}"
  probe="$candidate/tma1/ndnsf-di/$run_id"
  test ! -e "$probe" || continue
  if ! timeout 20 mkdir -p "$probe/home"; then
    continue
  fi
  if ! timeout 20 df -B1 "$probe" > "$evidence_parent/scratch-before.txt"; then
    find "$probe" -mindepth 1 -depth -delete 2>/dev/null || true
    rmdir "$probe" 2>/dev/null || true
    continue
  fi
  value="$(timeout 20 df -B1 --output=avail "$probe" |
    tail -1 | tr -d ' ')"
  case "$value" in
    ''|*[!0-9]*)
      find "$probe" -mindepth 1 -depth -delete 2>/dev/null || true
      rmdir "$probe" 2>/dev/null || true
      continue
      ;;
  esac
  scratch_root="$candidate"
  scratch="$probe"
  free_bytes="$value"
  printf 'scratchRoot=%s\nscratch=%s\nfreeBytes=%s\n' \
    "$scratch_root" "$scratch" "$free_bytes" \
    > "$evidence_parent/scratch-probe.txt"
  break
done

test -n "$scratch_root" || {
  echo "SPEC175_SCRATCH_PROBE_FAILED" >&2
  exit 75
}
test "$free_bytes" -gt $((120 * 1024 * 1024 * 1024)) || {
  echo "SPEC175_SCRATCH_FREE_BYTES_TOO_LOW:$free_bytes" >&2
  exit 75
}
