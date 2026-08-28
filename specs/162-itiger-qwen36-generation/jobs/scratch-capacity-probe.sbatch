#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=1
#SBATCH --mem=2G
#SBATCH --gres=gpu:rtx_5000:3
#SBATCH --time=00:05:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec162/capacity/slurm-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec162/capacity/slurm-%j.err

set -Eeuo pipefail
umask 077

: "${SPEC162_CAPACITY_SUBMISSION_ID:?}"
: "${SPEC162_CAPACITY_SOURCE_SHA256:?}"

readonly evidence_parent="/project/tma1/ndnsf-di/evidence/spec162/capacity"
readonly partial="${evidence_parent}/.${SPEC162_CAPACITY_SUBMISSION_ID}.partial"
readonly final="${evidence_parent}/${SPEC162_CAPACITY_SUBMISSION_ID}"
test ! -e "$partial"
test ! -e "$final"
mkdir -p "$partial"

terminal_state=FAIL
finish()
{
  rc=$?
  trap - EXIT INT TERM
  python3 - "$partial/result.json" "$terminal_state" "$rc" <<'PY' || true
import json
import os
import sys

path, state, return_code = sys.argv[1:]
with open(path, "w", encoding="utf-8") as stream:
    json.dump({
        "schemaVersion": "ndnsf-di-qwen36-scratch-probe-result-v1",
        "state": state,
        "exitCode": int(return_code),
        "jobId": os.environ["SLURM_JOB_ID"],
        "submissionId": os.environ["SPEC162_CAPACITY_SUBMISSION_ID"],
        "sourceSha256": os.environ["SPEC162_CAPACITY_SOURCE_SHA256"],
        "downloadPerformed": False,
        "modelComputePerformed": False,
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
trap finish EXIT INT TERM

comment=$(scontrol show job "$SLURM_JOB_ID" -o |
  tr ' ' '\n' | sed -n 's/^Comment=//p')
test "$comment" = "spec162:${SPEC162_CAPACITY_SUBMISSION_ID}"

nvidia-smi --query-gpu=index,uuid,name,memory.total \
  --format=csv,noheader > "$partial/gpu.csv"
test "$(wc -l < "$partial/gpu.csv")" -eq 3
test "$(grep -ci 'RTX 5000' "$partial/gpu.csv")" -eq 3

scratch_root=""
for candidate in "${SLURM_TMPDIR:-}" /scratch /tmp; do
  test -n "$candidate" || continue
  test -d "$candidate" || continue
  test -w "$candidate" || continue
  scratch_root=$candidate
  break
done
test -n "$scratch_root"

scratch_prefix="${scratch_root%/}/tma1/ndnsf-di/spec162-${SLURM_JOB_ID}"
case "$scratch_prefix" in
  /scratch/tma1/ndnsf-di/spec162-*|/tmp/tma1/ndnsf-di/spec162-*) ;;
  *)
    if test -z "${SLURM_TMPDIR:-}" ||
       test "$scratch_prefix" != \
         "${SLURM_TMPDIR%/}/tma1/ndnsf-di/spec162-${SLURM_JOB_ID}"; then
      echo "UNSAFE_SCRATCH_PREFIX:$scratch_prefix" >&2
      exit 4
    fi
    ;;
esac
test ! -e "$scratch_prefix"
mkdir -p "$scratch_prefix"

df -B1 "$scratch_prefix" > "$partial/scratch-df.txt"
findmnt -T "$scratch_prefix" > "$partial/scratch-findmnt.txt"
dd if=/dev/zero of="$scratch_prefix/fsync-64m.bin" \
  bs=1M count=64 conv=fsync status=none
sha256sum "$scratch_prefix/fsync-64m.bin" > "$partial/fsync-64m.sha256"
rm -f -- "$scratch_prefix/fsync-64m.bin"

python3 - "$partial/scratch-capacity.json" "$scratch_prefix" <<'PY'
import json
import os
import shutil
import socket
import sys

path, scratch = sys.argv[1:]
usage = shutil.disk_usage(scratch)
with open(path, "w", encoding="utf-8") as stream:
    json.dump({
        "schemaVersion": "ndnsf-di-qwen36-scratch-capacity-v1",
        "authority": "slurm-allocation",
        "jobId": os.environ["SLURM_JOB_ID"],
        "hostname": socket.gethostname(),
        "scratchPath": scratch,
        "scratchWritable": True,
        "scratchTotalBytes": usage.total,
        "scratchFreeBytes": usage.free,
        "scratchTemporaryPeakRequiredBytes": 171798691840,
        "fsyncProbeBytes": 64 * 1024 * 1024,
        "downloadPerformed": False,
        "modelComputePerformed": False,
    }, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY

find "$scratch_prefix" -mindepth 1 -delete
rmdir -- "$scratch_prefix"
terminal_state=PASS
