#!/usr/bin/env bash
set -euo pipefail

oci_reference=${1:?OCI digest reference required}
final_sif=${2:?final project SIF path required}
final_record=${3:?final project record path required}
printf '%s\n' "$oci_reference" | grep -Eq '^[^[:space:]]+@sha256:[a-f0-9]{64}$'

scratch=${SLURM_TMPDIR:-/tmp/ndnsf-di-${SLURM_JOB_ID:-$$}}
mkdir -p "$scratch/apptainer-cache" "$scratch/apptainer-tmp"
export APPTAINER_CACHEDIR="$scratch/apptainer-cache"
export APPTAINER_TMPDIR="$scratch/apptainer-tmp"
local_sif="$scratch/spec170-runtime.sif"
partial_sif="${final_sif}.partial"
partial_record="${final_record}.partial"
complete=false
cleanup() {
  if [ "$complete" != true ]; then
    if [ -n "${local_sha:-}" ]; then
      failure_record="${final_record}.promotion-failure-${SLURM_JOB_ID:-local}.json"
      python3 - "$failure_record" "$oci_reference" "$local_sif" "$local_sha" <<'PY' || true
import json, sys
path, oci, local_sif, local_sha = sys.argv[1:]
with open(path, "w", encoding="utf-8") as stream:
    json.dump({"schemaVersion": "ndnsf-sif-promotion-failure-v1",
               "ociReference": oci, "localSifPath": local_sif,
               "localSifSha256": "sha256:" + local_sha},
              stream, indent=2, sort_keys=True)
    stream.write("\n")
PY
    fi
    rm -f "$local_sif" "$partial_sif" "$partial_record"
  fi
}
trap cleanup EXIT INT TERM

test ! -e "$final_sif"; test ! -e "$final_record"
df -h "$scratch"
apptainer build "$local_sif" "docker://$oci_reference"
test -s "$local_sif"
local_sha=$(sha256sum "$local_sif" | cut -d' ' -f1)
printf 'LOCAL_SIF_SHA256=sha256:%s\n' "$local_sha"
local_size=$(stat -c '%s' "$local_sif")
printf 'LOCAL_SIF_SIZE=%s\n' "$local_size"

mkdir -p "$(dirname "$final_sif")" "$(dirname "$final_record")"
if cp "$local_sif" "$partial_sif"; then
  printf 'PROMOTE_CP_DONE\n'
else
  rc=$?
  printf 'PROMOTE_CP_FAIL rc=%s\n' "$rc" >&2
  exit "$rc"
fi
if sync; then
  printf 'PROMOTE_SYNC_DONE\n'
else
  rc=$?
  printf 'PROMOTE_SYNC_FAIL rc=%s\n' "$rc" >&2
  exit "$rc"
fi
promoted_size=$(stat -c '%s' "$partial_sif")
printf 'PROMOTED_PARTIAL_SIZE=%s\n' "$promoted_size"
promoted_sha=$(sha256sum "$partial_sif" | cut -d' ' -f1)
printf 'PROMOTED_PARTIAL_SHA256=sha256:%s\n' "$promoted_sha"
test "$promoted_sha" = "$local_sha"
test "$promoted_size" = "$local_size"
mv "$partial_sif" "$final_sif"
sync
test "$(sha256sum "$final_sif" | cut -d' ' -f1)" = "$local_sha"
printf 'PROMOTED_SIF_SHA256=sha256:%s\n' "$local_sha"

version=$(apptainer version)
python3 - "$partial_record" "$oci_reference" "$final_sif" "$local_sha" "$version" <<'PY'
import hashlib, json, sys
path, oci, sif, digest, version = sys.argv[1:]
body = {
    "schemaVersion": "ndnsf-sif-materialization-v2",
    "ociReference": oci,
    "ociDigest": "sha256:" + oci.rsplit("@sha256:", 1)[1],
    "sifPath": sif,
    "sifSha256": "sha256:" + digest,
    "apptainerVersion": version,
    "verified": True,
    "recoveredAfterPartialPromotion": False,
}
body["recordDigest"] = "sha256:" + hashlib.sha256(
    json.dumps(body, sort_keys=True, separators=(",", ":")).encode()
).hexdigest()
with open(path, "x", encoding="utf-8") as stream:
    json.dump(body, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY
mv "$partial_record" "$final_record"
"$(dirname "$0")/materialize-sif.sh" --oci-reference "$oci_reference" --sif "$final_sif" --record "$final_record"
complete=true
trap - EXIT INT TERM
printf 'MATERIALIZATION_LOCAL_SCRATCH_PASS sif=%s\n' "$final_sif"
