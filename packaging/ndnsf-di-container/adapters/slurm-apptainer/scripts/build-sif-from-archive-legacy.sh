#!/usr/bin/env bash
# Historical OCI-to-SIF fallback only.  This file is deliberately named
# ``*-legacy`` so it cannot be mistaken for the normal local-SIF entry point.
#
# The current Spec170 release route starts with a complete application SIF
# built directly by host Apptainer (definition or sealed localimage refresh).
# It does not create an OCI archive and does not invoke this helper. Keep this
# script only for an explicitly authorized legacy conversion, with a separate
# release identity; never use its output as the current route by implication.
set -euo pipefail

usage() {
  echo "usage: $0 --archive PATH --sif PATH --record PATH --source-seal PATH [--archive-format oci-archive|docker-archive] [--remote-host HOST] [--expected-apptainer VERSION]" >&2
  exit 2
}

archive=''
sif=''
record=''
source_seal=''
archive_format=auto
remote_host=itiger
expected_version=''
while (($#)); do
  case "$1" in
    --archive) archive=$2; shift 2 ;;
    --sif) sif=$2; shift 2 ;;
    --record) record=$2; shift 2 ;;
    --source-seal) source_seal=$2; shift 2 ;;
    --archive-format) archive_format=$2; shift 2 ;;
    --remote-host) remote_host=$2; shift 2 ;;
    --expected-apptainer) expected_version=$2; shift 2 ;;
    *) usage ;;
  esac
done

[ -f "$archive" ] || { echo LOCAL_SIF_ARCHIVE_MISSING >&2; exit 4; }
[ -f "$source_seal" ] || { echo LOCAL_SIF_SOURCE_SEAL_MISSING >&2; exit 4; }
[ -n "$sif" ] && [ -n "$record" ] || usage
case "$archive_format" in oci-archive|docker-archive|auto) ;; *) usage ;; esac

normalize_version() { printf '%s\n' "$1" | sed -E 's/[^0-9]*([0-9]+\.[0-9]+\.[0-9]+).*/\1/'; }
local_version=$(apptainer version)
if [ -z "$expected_version" ]; then
  expected_version=$(ssh -o BatchMode=yes -o ConnectTimeout=10 "$remote_host" 'apptainer version')
fi
[ "$(normalize_version "$local_version")" = "$(normalize_version "$expected_version")" ] || {
  echo "LOCAL_SIF_APPTAINER_VERSION_MISMATCH local=$local_version remote=$expected_version" >&2
  exit 4
}

if [ "$archive_format" = auto ]; then
  case "$archive" in
    *.oci.tar|*.oci-archive) archive_format=oci-archive ;;
    *) archive_format=docker-archive ;;
  esac
fi
archive_ref="$archive_format:$archive"
mkdir -p "$(dirname "$sif")" "$(dirname "$record")"
partial="$sif.partial"
record_partial="$record.partial"
[ ! -e "$sif" ] || { echo LOCAL_SIF_OUTPUT_EXISTS >&2; exit 4; }
[ ! -e "$record" ] || { echo LOCAL_SIF_RECORD_EXISTS >&2; exit 4; }
rm -f "$partial" "$record_partial"
archive_sha256=$(sha256sum "$archive" | awk '{print $1}')
source_seal_sha256=$(sha256sum "$source_seal" | awk '{print $1}')
trap 'rm -f "$partial" "$record_partial"' EXIT INT TERM
apptainer build "$partial" "$archive_ref"
[ -s "$partial" ] || { echo LOCAL_SIF_EMPTY >&2; exit 4; }
mv "$partial" "$sif"
sif_sha256=$(sha256sum "$sif" | awk '{print $1}')
python3 - "$record_partial" "$archive" "$archive_format" "$archive_sha256" "$source_seal" "$source_seal_sha256" "$sif" "$sif_sha256" "$local_version" "$expected_version" <<'PY'
import hashlib, json, os, sys
(path, archive, archive_format, archive_sha, source_seal, source_sha,
 sif, sif_sha, local_version, expected_version) = sys.argv[1:]
body = {
    'schemaVersion': 'ndnsf-local-sif-build-v1',
    'status': 'PASS',
    'archive': {'path': archive, 'format': archive_format, 'sha256': 'sha256:' + archive_sha},
    'sourceSeal': {'path': source_seal, 'sha256': 'sha256:' + source_sha},
    'sif': {'path': sif, 'sha256': 'sha256:' + sif_sha, 'bytes': os.path.getsize(sif)},
    'apptainer': {'local': local_version, 'remote': expected_version},
    'buildHostOnly': True,
    'tigerAction': 'verify-hash-and-execute-only',
}
body['recordDigest'] = 'sha256:' + hashlib.sha256(json.dumps(body, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
with open(path, 'x', encoding='utf-8') as stream:
    json.dump(body, stream, indent=2, sort_keys=True)
    stream.write('\n')
PY
mv "$record_partial" "$record"
trap - EXIT INT TERM
printf 'LOCAL_SIF_BUILD_PASS sif=%s sha256:%s apptainer=%s\n' "$sif" "$sif_sha256" "$local_version"
