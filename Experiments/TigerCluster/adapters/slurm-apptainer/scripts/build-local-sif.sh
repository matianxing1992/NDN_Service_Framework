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
  --host-gate-manifest PATH \
  [--strict-host-source-seal] \
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
host_gate_manifest=''
strict_host_source_seal=0
apptainer_bin=''
expected_version=''

while (($#)); do
  case "$1" in
    --definition) definition=${2:-}; shift 2 ;;
    --sif) sif=${2:-}; shift 2 ;;
    --record) record=${2:-}; shift 2 ;;
    --source-seal) source_seal=${2:-}; shift 2 ;;
    --host-gate-manifest) host_gate_manifest=${2:-}; shift 2 ;;
    --strict-host-source-seal) strict_host_source_seal=1; shift ;;
    --apptainer) apptainer_bin=${2:-}; shift 2 ;;
    --expected-apptainer) expected_version=${2:-}; shift 2 ;;
    *) usage ;;
  esac
done

[ -n "$definition" ] && [ -n "$sif" ] && [ -n "$record" ] && \
  [ -n "$source_seal" ] && [ -n "$apptainer_bin" ] && \
  [ -n "$host_gate_manifest" ] && \
  [ -n "$expected_version" ] || usage
[ -f "$definition" ] || { echo LOCAL_SIF_DEFINITION_MISSING >&2; exit 4; }
[ -f "$source_seal" ] || { echo LOCAL_SIF_SOURCE_SEAL_MISSING >&2; exit 4; }
[ -f "$host_gate_manifest" ] || { echo LOCAL_SIF_HOST_GATE_MANIFEST_MISSING >&2; exit 4; }
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
repository_root=$(CDPATH= cd -- "$script_dir/../../../../.." && pwd)
boundary_validator="$script_dir/../../../lib/spec170_sif_build_boundary.py"
host_gate_validator="$script_dir/../../../lib/spec175_host_gate.py"
source_validator="$script_dir/validate-local-sif-source.py"
spec175_preflight="$script_dir/../../../bin/ndnsf-di-spec175-preflight"
spec175_workload="$script_dir/../../../jobs/spec175/workload.json"
[ -f "$boundary_validator" ] || {
  echo LOCAL_SIF_BUILD_BOUNDARY_VALIDATOR_MISSING >&2
  exit 4
}
[ -f "$source_validator" ] || {
  echo LOCAL_SIF_SOURCE_VALIDATOR_MISSING >&2
  exit 4
}
[ -f "$host_gate_validator" ] || {
  echo SPEC175_HOST_GATE_VALIDATOR_MISSING >&2
  exit 4
}
[ -x "$spec175_preflight" ] || {
  echo SPEC175_PREFLIGHT_MISSING >&2
  exit 4
}
[ -f "$spec175_workload" ] || {
  echo SPEC175_WORKLOAD_MISSING >&2
  exit 4
}
if ! source_validation_json=$(python3 "$source_validator" --source-seal "$source_seal"); then
  exit 4
fi
if ! python3 - "$definition" "$source_seal" <<'PY'
import json
import sys

definition, source_seal = sys.argv[1:]
declared = None
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
        fields = line.split(None, 1)
        if len(fields) == 2 and fields[0] == "org.ndnsf.di.source-seal":
            declared = fields[1]
expected = json.loads(open(source_seal, encoding="utf-8").read()).get(
    "sealDigest", "")
if declared is not None and declared != expected:
    print(
        "LOCAL_SIF_DEFINITION_SOURCE_SEAL_LABEL_MISMATCH "
        f"expected={expected} actual={declared}",
        file=sys.stderr,
    )
    raise SystemExit(4)
PY
then
  exit 4
fi
if ! host_gate_json=$(python3 - "$host_gate_validator" "$host_gate_manifest" "$repository_root" <<'PY'
import importlib.util
import sys
from pathlib import Path

module_path, manifest_path, repository_root = sys.argv[1:]
spec = importlib.util.spec_from_file_location("spec175_host_gate", module_path)
if spec is None or spec.loader is None:
    raise SystemExit("SPEC175_HOST_GATE_VALIDATOR_IMPORT_FAILED")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
try:
    result = module.validate_host_gate(Path(manifest_path), Path(repository_root))
except Exception as exc:
    print(str(exc), file=sys.stderr)
    raise SystemExit(4)
import json
print(json.dumps(result, sort_keys=True))
PY
); then
  exit 4
fi
# The host G3 manifest is a qualification of the exact source identity being
# built.  A structurally valid 30/30 manifest from an older source seal must
# not be silently reused for a source-different SIF candidate.  The Spec175
# source seal intentionally permits descendant document-only commits and may
# be clean (``dirtyFiles`` is then empty), so requiring a dirty-file overlap
# would reject the normal clean-tree build.
if [ "$strict_host_source_seal" = 1 ] && ! python3 - "$host_gate_json" "$source_seal" "$repository_root" <<'PY'
import importlib.util
import json
import subprocess
import sys
from pathlib import Path

host = json.loads(sys.argv[1])
local_source_path = Path(sys.argv[2])
local_source = json.loads(local_source_path.read_text(encoding="utf-8"))
repository_root = Path(sys.argv[3])
host_path = Path(host["sourceSealPath"])
if not host_path.is_absolute():
    host_path = repository_root / host_path
try:
    gate_path = repository_root / "scripts/spec175_contract_gate.py"
    spec = importlib.util.spec_from_file_location("spec175_contract_gate", gate_path)
    if spec is None or spec.loader is None:
        raise RuntimeError("SPEC175_CONTRACT_GATE_IMPORT_FAILED")
    gate = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(gate)
    current_revision, all_dirty = gate._git_state(repository_root)
    host_record, issues = gate._verify_source_seal(
        repository_root, host_path, current_revision, all_dirty)
except Exception as error:
    print(f"LOCAL_SIF_HOST_GATE_SOURCE_SEAL_CHECK_FAILED error={error}", file=sys.stderr)
    raise SystemExit(4)
if issues:
    issue = issues[0]
    print(
        "LOCAL_SIF_HOST_GATE_SOURCE_SEAL_INVALID "
        f"code={issue.get('code', 'UNKNOWN')} detail={issue.get('detail', '')}",
        file=sys.stderr,
    )
    raise SystemExit(4)

local_revision = str(local_source.get("sourceRevision", ""))
if not local_revision:
    print("LOCAL_SIF_SOURCE_SEAL_REVISION_MISSING", file=sys.stderr)
    raise SystemExit(4)
source_changes = gate._source_changes_between(
    repository_root, local_revision, current_revision)
if source_changes is None:
    print(
        "LOCAL_SIF_SOURCE_SEAL_REVISION_NOT_ANCESTOR "
        f"archive={local_revision} current={current_revision}",
        file=sys.stderr,
    )
    raise SystemExit(4)
if source_changes:
    print(
        "LOCAL_SIF_SOURCE_SEAL_SOURCE_DRIFT "
        + ", ".join(source_changes[:12]),
        file=sys.stderr,
    )
    raise SystemExit(4)

previous = json.loads(host_path.read_text(encoding="utf-8"))
local_rows = {row["path"]: row.get("sha256") for row in local_source.get("files", [])}
# The G0/G3 source seal covers the whole Spec175 qualification subject, while
# workspace.tar intentionally contains only source needed to build/runtime the
# SIF.  Tests, host preflights, and submission/checklist tooling are host-only;
# the replay driver and workload are the explicit packaging-owned exceptions
# that must remain in the archive.  Do not silently allow an omitted runtime
# path: an unclassified omission is still a hard candidate failure.
host_only_prefixes = (
    "tests/",
    "packaging/ndnsf-di-container/",
)
runtime_archive_paths = {
    "packaging/ndnsf-di-container/jobs/spec175/replay-exact-sif.py",
    "packaging/ndnsf-di-container/jobs/spec175/workload.json",
}
for path, row in previous.get("dirtyFiles", {}).items():
    expected = row.get("sha256")
    archived = local_rows.get(path)
    if expected and archived == expected:
        continue
    if (path.startswith(host_only_prefixes)
            and path not in runtime_archive_paths
            and archived is None):
        continue
    if expected:
        print(
            "LOCAL_SIF_HOST_GATE_SOURCE_FILE_MISMATCH "
            f"path={path} expected={expected} archive={archived}",
            file=sys.stderr,
        )
        raise SystemExit(4)
PY
then
  exit 4
fi
if ! spec175_input_preflight_json=$(python3 "$spec175_preflight" \
    --source-seal "$source_seal" --workload "$spec175_workload"); then
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
if ! ndnsf_labels_json=$(python3 - "$definition" "$inspect_json" "$source_seal" <<'PY'
import json
import sys

definition, inspect_json, source_seal = sys.argv[1:]
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
try:
    seal_digest = json.loads(open(source_seal, encoding="utf-8").read()).get("sealDigest", "")
except (OSError, json.JSONDecodeError) as error:
    print(f"LOCAL_SIF_SOURCE_SEAL_READ_FAILED error={error}", file=sys.stderr)
    raise SystemExit(4)
if actual.get("org.ndnsf.di.source-seal") != seal_digest:
    print(
        "LOCAL_SIF_SOURCE_SEAL_LABEL_MISMATCH "
        f"expected={seal_digest} actual={actual.get('org.ndnsf.di.source-seal')}",
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

# The source-only check above is necessary but insufficient.  Before a build
# record can be emitted, inspect the actual candidate SIF and execute the
# native ABI/ONNX probe inside it.  This is the boundary that rejects stale
# host-built extensions, missing ldd dependencies, CPU-only ORT wheels, and
# deployment-time PyTorch/Transformers residue.
if ! spec175_preflight_json=$(python3 "$spec175_preflight" \
    --source-seal "$source_seal" --workload "$spec175_workload" \
    --sif "$sif" --apptainer "$apptainer_bin" \
    --expected-sif-sha256 "$sif_sha256"); then
  exit 4
fi

python3 - "$record_partial" "$definition" "$definition_sha256" "$source_seal" \
  "$source_seal_sha256" "$sif" "$sif_sha256" "$local_version" "$expected_version" \
  "$base_sif" "$base_sif_sha256" "$base_sif_bytes" "$ndnsf_labels_json" \
  "$apptainer_bin" "$apptainer_sha256" "$boundary_json" \
  "$source_validation_json" "$host_gate_json" "$spec175_input_preflight_json" \
  "$spec175_preflight_json" <<'PY'
import hashlib
import json
import os
import sys

(path, definition, definition_sha, source_seal, source_sha,
 sif, sif_sha, local_version, expected_version,
 base_sif, base_sif_sha, base_sif_bytes, labels_json,
 apptainer_bin, apptainer_sha, boundary_json, source_validation_json,
 host_gate_json, spec175_input_preflight_json,
 spec175_preflight_json) = sys.argv[1:]
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
    "hostGate": json.loads(host_gate_json),
    "spec175InputPreflight": json.loads(spec175_input_preflight_json),
    "spec175Preflight": json.loads(spec175_preflight_json),
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
