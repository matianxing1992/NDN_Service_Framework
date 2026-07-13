#!/bin/bash
set -euo pipefail
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/spec110-security.XXXXXX")
trap 'rm -rf "$tmp"' EXIT INT TERM
launcher="$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/probe-ndnsf-security.sh"
fixture="$repo/tests/container/itiger-qwen-live/fixtures/network/multi-node-tcp.json"
"$launcher" --process-map "$fixture" --source-root "$repo" --evidence "$tmp/evidence" --mode contract \
  | grep -q SPEC110_SECURITY_CONTRACT_PASS
python3 - "$tmp/evidence/security-manifest.json" <<'PY'
import json,sys
value=json.load(open(sys.argv[1]))
assert value['status']=='CONTRACT_READY'
assert value['placementClass']=='multi-node'
assert len(value['roles']['nfd'])==2
assert len(value['roles']['provider'])==3
assert len(value['regressions'])==6
assert all(row['sha256'].startswith('sha256:') and row['status']=='NOT_EXECUTED' for row in value['regressions'])
assert value['checks']=={'permissionDistribution':'REQUIRED','nacAbeRouting':'REQUIRED','userProviderTokens':'REQUIRED','replayRejection':'REQUIRED','customSelection':'REQUIRED'}
PY
if NDNSF_SPEC110_SECURITY_EXECUTION=0 "$launcher" --process-map "$fixture" --source-root "$repo" --evidence "$tmp/not-armed" --mode execute; then
  echo SECURITY_EXECUTION_GUARD_UNEXPECTED_PASS >&2; exit 1
fi
printf 'PACKAGED_SECURITY_CONTRACT_PASS\n'
