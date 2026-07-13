#!/bin/bash
set -euo pipefail

usage() {
  echo "usage: $0 --process-map FILE --source-root DIR --evidence DIR --mode contract|execute" >&2
  exit 2
}
process_map= source_root= evidence= mode=
while (($#)); do
  case "$1" in
    --process-map) process_map=$2; shift 2 ;;
    --source-root) source_root=$2; shift 2 ;;
    --evidence) evidence=$2; shift 2 ;;
    --mode) mode=$2; shift 2 ;;
    *) usage ;;
  esac
done
[[ -f $process_map && -d $source_root && -n $evidence ]] || usage
[[ $mode == contract || $mode == execute ]] || usage

container_root=$(CDPATH= cd -- "$(dirname -- "$0")/../../.." && pwd)
lib="$container_root/lib"; mkdir -p "$evidence/logs"
manifest="$evidence/security-manifest.json"
regressions=(
  examples/run_hello_auth_regression.sh
  examples/run_hello_ack_payload_regression.sh
  examples/run_selective_ack_custom_selection_regression.sh
  examples/run_nac_abe_attribute_routing_regression.sh
  examples/run_token_handshake_negative_regression.sh
  examples/run_token_certificate_bootstrap_regression.sh
)
for regression in "${regressions[@]}"; do [[ -x $source_root/$regression ]] || { echo "SECURITY_REGRESSION_MISSING:$regression" >&2; exit 4; }; done

PYTHONPATH="$lib" python3 - "$process_map" "$source_root" "$manifest" "${regressions[@]}" <<'PY'
import hashlib,json,sys
from pathlib import Path
from allocation_topology import load_process_map
process_map=load_process_map(sys.argv[1]);root=Path(sys.argv[2]);output=Path(sys.argv[3]);regressions=sys.argv[4:]
roles={row['kind']:[] for row in process_map['processes']}
for row in process_map['processes']: roles[row['kind']].append({'processId':row['processId'],'identityRef':row['identityRef'],'nodeRank':row['nodeRank'],'readOnly':row['identityReadOnly']})
body={'schemaVersion':'spec110-packaged-security-v1','status':'CONTRACT_READY','placementClass':process_map['placementClass'],'selectedTransport':process_map['selectedTransport'],'roles':roles,'checks':{'permissionDistribution':'REQUIRED','nacAbeRouting':'REQUIRED','userProviderTokens':'REQUIRED','replayRejection':'REQUIRED','customSelection':'REQUIRED'},'regressions':[]}
for name in regressions:
 path=root/name;body['regressions'].append({'path':name,'sha256':'sha256:'+hashlib.sha256(path.read_bytes()).hexdigest(),'status':'NOT_EXECUTED'})
output.write_text(json.dumps(body,indent=2,sort_keys=True)+'\n')
PY

if [[ $mode == contract ]]; then
  echo SPEC110_SECURITY_CONTRACT_PASS
  exit 0
fi
[[ ${NDNSF_SPEC110_SECURITY_EXECUTION:-0} == 1 ]] || { echo SECURITY_EXECUTION_NOT_ARMED >&2; exit 3; }
[[ -n ${NDN_CLIENT_TRANSPORT:-} || -S /run/nfd/nfd.sock ]] || { echo SECURITY_NFD_UNAVAILABLE >&2; exit 3; }

status=0
for regression in "${regressions[@]}"; do
  name=$(basename "$regression" .sh)
  started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
  set +e
  timeout 180s "$source_root/$regression" >"$evidence/logs/$name.log" 2>&1
  rc=$?
  set -e
  finished=$(date -u +%Y-%m-%dT%H:%M:%SZ)
  printf '{"path":"%s","startedAt":"%s","finishedAt":"%s","exitCode":%d}\n' \
    "$regression" "$started" "$finished" "$rc" >"$evidence/logs/$name.result.json"
  [[ $rc -eq 0 ]] || status=1
done

python3 - "$manifest" "$evidence/logs" "$status" <<'PY'
import json,sys
from pathlib import Path
manifest=Path(sys.argv[1]);logs=Path(sys.argv[2]);failed=int(sys.argv[3]);value=json.loads(manifest.read_text())
results={json.loads(path.read_text())['path']:json.loads(path.read_text()) for path in logs.glob('*.result.json')}
for row in value['regressions']:
 result=results[row['path']];row.update(status='PASS' if result['exitCode']==0 else 'FAIL',**{k:v for k,v in result.items() if k!='path'})
value['status']='PASS' if failed==0 else 'FAIL';manifest.write_text(json.dumps(value,indent=2,sort_keys=True)+'\n')
PY
[[ $status -eq 0 ]] || { echo SPEC110_SECURITY_REGRESSIONS_FAIL >&2; exit 5; }
echo SPEC110_SECURITY_REGRESSIONS_PASS
