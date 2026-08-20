#!/usr/bin/env bash
# Build the complete NDNSF-DI application SIF on the local host.
#
# This is the normal Spec170 release entry point.  The definition may use
# ``Bootstrap: localimage`` for a sealed, qualified base SIF, but the final
# application SIF is always built and verified here.  Docker/OCI archives and
# Tiger-side materialization are intentionally not accepted by this script.
set -euo pipefail

usage() {
  cat >&2 <<'EOF'
usage: build-local-sif.sh \
  --definition PATH --sif PATH --record PATH --source-seal PATH \
  --apptainer PATH --expected-apptainer VERSION

The definition is executed by the local host's Apptainer.  It may bootstrap
from a sealed localimage, but must produce the complete application SIF.
The expected version must come from a bounded Slurm compute-node probe, not
from the Tiger login node.
EOF
  exit 2
}

definition=''
sif=''
record=''
source_seal=''
apptainer_bin=''
expected_version=''

while (($#)); do
  case "$1" in
    --definition) definition=${2:-}; shift 2 ;;
    --sif) sif=${2:-}; shift 2 ;;
    --record) record=${2:-}; shift 2 ;;
    --source-seal) source_seal=${2:-}; shift 2 ;;
    --apptainer) apptainer_bin=${2:-}; shift 2 ;;
    --expected-apptainer) expected_version=${2:-}; shift 2 ;;
    *) usage ;;
  esac
done

[ -n "$definition" ] && [ -n "$sif" ] && [ -n "$record" ] && \
  [ -n "$source_seal" ] && [ -n "$apptainer_bin" ] && \
  [ -n "$expected_version" ] || usage
[ -f "$definition" ] || { echo LOCAL_SIF_DEFINITION_MISSING >&2; exit 4; }
[ -f "$source_seal" ] || { echo LOCAL_SIF_SOURCE_SEAL_MISSING >&2; exit 4; }
[ ! -e "$sif" ] || { echo LOCAL_SIF_OUTPUT_EXISTS >&2; exit 4; }
[ ! -e "$record" ] || { echo LOCAL_SIF_RECORD_EXISTS >&2; exit 4; }
[ -x "$apptainer_bin" ] || { echo LOCAL_SIF_APPTAINER_NOT_EXECUTABLE >&2; exit 4; }
apptainer_bin=$(readlink -f "$apptainer_bin")

normalize_version() {
  printf '%s\n' "$1" | sed -E 's/[^0-9]*([0-9]+\.[0-9]+\.[0-9]+).*/\1/'
}

local_version=$("$apptainer_bin" version)
[ "$(normalize_version "$local_version")" = "$(normalize_version "$expected_version")" ] || {
  echo "LOCAL_SIF_APPTAINER_VERSION_MISMATCH local=$local_version compute=$expected_version" >&2
  exit 4
}
apptainer_sha256=$(sha256sum "$apptainer_bin" | awk '{print $1}')
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
boundary_validator="$script_dir/../../../lib/spec170_sif_build_boundary.py"
source_validator="$script_dir/validate-local-sif-source.py"
[ -f "$boundary_validator" ] || {
  echo LOCAL_SIF_BUILD_BOUNDARY_VALIDATOR_MISSING >&2
  exit 4
}
[ -f "$source_validator" ] || {
  echo LOCAL_SIF_SOURCE_VALIDATOR_MISSING >&2
  exit 4
}
if ! source_validation_json=$(python3 "$source_validator" --source-seal "$source_seal"); then
  exit 4
fi
if ! boundary_json=$(python3 "$boundary_validator" --definition "$definition"); then
  exit 4
fi

mkdir -p "$(dirname "$sif")" "$(dirname "$record")"
partial="$sif.partial"
record_partial="$record.partial"
rm -f "$partial" "$record_partial"
trap 'rm -f "$partial" "$record_partial"' EXIT INT TERM

definition_sha256=$(sha256sum "$definition" | awk '{print $1}')
source_seal_sha256=$(sha256sum "$source_seal" | awk '{print $1}')

bootstrap=$(awk -F: '
  tolower($1) ~ /^[[:space:]]*bootstrap[[:space:]]*$/ {
    value=$2; gsub(/^[[:space:]]+|[[:space:]]+$/, "", value); print tolower(value); exit
  }' "$definition")
base_sif=''
base_sif_sha256=''
base_sif_bytes='0'
if [ "$bootstrap" = localimage ]; then
  base_sif=$(awk -F: '
    tolower($1) ~ /^[[:space:]]*from[[:space:]]*$/ {
      value=substr($0, index($0, ":") + 1)
      gsub(/^[[:space:]]+|[[:space:]]+$/, "", value); print value; exit
    }' "$definition")
  [ -n "$base_sif" ] || { echo LOCAL_SIF_BASE_MISSING_FROM >&2; exit 4; }
  case "$base_sif" in
    /*) ;;
    *) echo "LOCAL_SIF_BASE_PATH_NOT_ABSOLUTE path=$base_sif" >&2; exit 4 ;;
  esac
  [ -f "$base_sif" ] || { echo "LOCAL_SIF_BASE_MISSING path=$base_sif" >&2; exit 4; }
  base_sif=$(readlink -f "$base_sif")
  base_sif_sha256=$(sha256sum "$base_sif" | awk '{print $1}')
  base_sif_bytes=$(stat -c '%s' "$base_sif")
fi

echo "LOCAL_SIF_BUILD_START definition=$definition output=$sif apptainer=$local_version binary=$apptainer_bin"
"$apptainer_bin" build --force "$partial" "$definition"
[ -s "$partial" ] || { echo LOCAL_SIF_EMPTY >&2; exit 4; }
inspect_json=$("$apptainer_bin" inspect --json "$partial")
if ! ndnsf_labels_json=$(python3 - "$definition" "$inspect_json" <<'PY'
import json
import sys

definition, inspect_json = sys.argv[1:]
expected = {}
in_labels = False
with open(definition, encoding="utf-8") as stream:
    for raw in stream:
        line = raw.strip()
        if line == "%labels":
            in_labels = True
            continue
        if line.startswith("%"):
            in_labels = False
        if not in_labels or not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        if len(parts) == 2 and parts[0].startswith("org.ndnsf.di."):
            expected[parts[0]] = parts[1]

actual = json.loads(inspect_json)["data"]["attributes"]["labels"]
mismatches = [
    (key, value, actual.get(key))
    for key, value in sorted(expected.items())
    if actual.get(key) != value
]
if mismatches:
    for key, expected_value, actual_value in mismatches:
        print(
            f"LOCAL_SIF_LABEL_MISMATCH key={key} "
            f"expected={expected_value} actual={actual_value}",
            file=sys.stderr,
        )
    raise SystemExit(4)
print(json.dumps({key: actual[key] for key in sorted(expected)}, sort_keys=True))
PY
); then
  exit 4
fi
mv "$partial" "$sif"
sif_sha256=$(sha256sum "$sif" | awk '{print $1}')

python3 - "$record_partial" "$definition" "$definition_sha256" "$source_seal" \
  "$source_seal_sha256" "$sif" "$sif_sha256" "$local_version" "$expected_version" \
  "$base_sif" "$base_sif_sha256" "$base_sif_bytes" "$ndnsf_labels_json" \
  "$apptainer_bin" "$apptainer_sha256" "$boundary_json" \
  "$source_validation_json" <<'PY'
import hashlib
import json
import os
import sys

(path, definition, definition_sha, source_seal, source_sha,
 sif, sif_sha, local_version, expected_version,
 base_sif, base_sif_sha, base_sif_bytes, labels_json,
 apptainer_bin, apptainer_sha, boundary_json, source_validation_json) = sys.argv[1:]
build_input = {
    "definition": {"path": definition, "sha256": "sha256:" + definition_sha},
    "method": "local-apptainer-definition",
}
if base_sif:
    build_input["baseSif"] = {
        "path": base_sif,
        "sha256": "sha256:" + base_sif_sha,
        "bytes": int(base_sif_bytes),
    }
body = {
    "schemaVersion": "ndnsf-local-sif-build-v3",
    "status": "PASS",
    "buildInput": build_input,
    "sourceSeal": {"path": source_seal, "sha256": "sha256:" + source_sha},
    "sourceValidation": json.loads(source_validation_json),
    "sif": {"path": sif, "sha256": "sha256:" + sif_sha,
            "bytes": os.path.getsize(sif)},
    "labels": json.loads(labels_json),
    "apptainer": {
        "local": local_version,
        "computeExpected": expected_version,
        "remote": expected_version,
        "path": apptainer_bin,
        "sha256": "sha256:" + apptainer_sha,
    },
    "hostRole": "apptainer-driver-only",
    "containerNativeBuild": json.loads(boundary_json),
    "tigerAction": "verify-hash-and-execute-only",
}
body["recordDigest"] = "sha256:" + hashlib.sha256(
    json.dumps(body, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
with open(path, "x", encoding="utf-8") as stream:
    json.dump(body, stream, indent=2, sort_keys=True)
    stream.write("\n")
PY

mv "$record_partial" "$record"

trap - EXIT INT TERM
printf 'LOCAL_SIF_BUILD_PASS sif=%s sha256:%s apptainer=%s\n' \
  "$sif" "$sif_sha256" "$local_version"
