#!/bin/bash
set -euo pipefail
repo=$(CDPATH= cd -- "$(dirname -- "$0")/../../../.." && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/spec110-network.XXXXXX")
supervisor_scratch=$(mktemp -d /tmp/ndnsf-di-supervisor.XXXXXX)
cleanup() {
  rc=$?
  if [[ ${NDNSF_SPEC110_KEEP_TEST_TMP:-0} == 1 && $rc -ne 0 ]]; then
    echo "NETWORK_TEST_TMP=$tmp SUPERVISOR_SCRATCH=$supervisor_scratch" >&2
  else
    rm -rf "$tmp" "$supervisor_scratch"
  fi
}
trap cleanup EXIT INT TERM
fixture="$repo/tests/container/itiger-qwen-live/fixtures/network/multi-node-tcp.json"
variants="$repo/tests/container/itiger-qwen-live/fixtures/network/variants.json"

python3 - "$variants" "$tmp/observation.json" <<'PY'
import json,sys
value=json.load(open(sys.argv[1]))['probeObservations']['tcp-pass-udp-diagnostic-fail']
json.dump(value,open(sys.argv[2],'w'))
PY
SLURM_JOB_ID=test NDNSF_SPEC110_TEST_MODE=1 NDNSF_SPEC110_PROBE_OBSERVATION="$tmp/observation.json" \
  "$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/probe-multinode-network.sh" \
  --process-map "$fixture" --output "$tmp/probe.json"
python3 - "$tmp/probe.json" <<'PY'
import json,sys
value=json.load(open(sys.argv[1]))
assert value['verdict']['status']=='PASS'
assert value['verdict']['selectedTransport']=='tcp'
assert value['verdict']['diagnosticStatus']=='FAIL'
PY

mkdir -p "$tmp/bin"
cat >"$tmp/bin/srun" <<'SH'
#!/bin/bash
set -e
while (($#)) && [[ $1 == --* ]]; do
  case "$1" in
    --export=ALL,*)
      IFS=, read -ra exports <<<"${1#--export=ALL,}"
      for item in "${exports[@]}"; do export "$item"; done
      ;;
  esac
  shift
done
if [[ ${1:-} == env ]]; then
  shift
  while (($#)) && [[ $1 == *=* ]]; do export "$1"; shift; done
fi
if [[ ${1:-} == test && ${2:-} == -S ]]; then
  test -e "$3"
  exit $?
fi
exec "$@"
SH
cat >"$tmp/bin/nfdc" <<'SH'
#!/bin/sh
printf 'fake-nfdc %s\n' "$*"
SH
chmod 0755 "$tmp/bin/srun" "$tmp/bin/nfdc"
for repetition in 1 2; do
  PATH="$tmp/bin:$PATH" SLURM_JOB_ID=test NDNSF_SPEC110_TEST_MODE=1 \
    "$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/configure-allocation-routes.sh" \
    --process-map "$fixture" --evidence "$tmp/routes"
done
grep -q '^ROUTE_CONFIGURATION_PASS$' "$tmp/routes/route-verdict.txt"
grep -q 'face create remote tcp4://10.10.0.11:16364 persistency permanent' "$tmp/routes/route-commands.log"
grep -q 'route add prefix /spec110/node/1 nexthop tcp4://10.10.0.11:16364 cost 10' "$tmp/routes/nfdc-route.log"

cat >"$tmp/bin/nfd" <<'SH'
#!/bin/bash
set -euo pipefail
[[ $1 == --config ]]
socket_path=$(awk '$1=="path" {print $2;exit}' "$2")
mkdir -p "$(dirname "$socket_path")"
touch "$socket_path"
trap 'rm -f "$socket_path"; exit 0' TERM INT EXIT
while :; do sleep 1; done
SH
cat >"$tmp/bin/App_ServiceController" <<'SH'
#!/bin/bash
echo NDNSF_DI_CONTROLLER_READY
trap 'exit 0' TERM INT
while :; do sleep 1; done
SH
cat >"$tmp/bin/di-native-provider" <<'SH'
#!/bin/bash
echo NDNSF_DI_NATIVE_PROVIDER_READY
trap 'exit 0' TERM INT
while :; do sleep 1; done
SH
cat >"$tmp/bin/spec110-fake-user" <<'SH'
#!/bin/sh
echo SPEC110_FAKE_USER_TERMINAL
SH
chmod 0755 "$tmp/bin/nfd" "$tmp/bin/App_ServiceController" "$tmp/bin/di-native-provider" "$tmp/bin/spec110-fake-user"

python3 - "$repo" "$fixture" "$supervisor_scratch/process-map.json" "$supervisor_scratch" <<'PY'
import json,sys
sys.path.insert(0,sys.argv[1]+'/packaging/ndnsf-di-container/lib')
from allocation_topology import command_digest
value=json.load(open(sys.argv[2]));scratch=sys.argv[4];socket=scratch+'/nfd/0/nfd.sock'
value['placementClass']='single-node-multi-gpu';value['nodes']=value['nodes'][:1];value['nodes'][0].update(address='127.0.0.1',nfdSocket=socket,tcpPort=16363,udpPort=16363);value['routes']=[]
kept=[]
for process in value['processes']:
 if process['kind']=='nfd' and process['nodeRank']!=0:continue
 if process['kind'] in {'provider','user'}:process['nodeRank']=0
 if process['kind']=='provider':process['gpuRank']=int(process['role'].rsplit('-',1)[1])
 if process['kind']=='controller':process['readinessInputs']=['nfd-0-ready']
 process['nfdSocket']=socket
 kept.append(process)
value['processes']=kept
for rank,process in enumerate(value['processes']):
 process['taskRank']=rank
 if process['kind']=='nfd':process['command']=['nfd','--config',scratch+'/nfd/0/nfd.conf']
 if process['kind']=='user':process['command']=['spec110-fake-user']
 process['commandDigest']=command_digest(process['command'])
json.dump(value,open(sys.argv[3],'w'))
PY

supervisor="$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-allocation-topology.sh"
template="$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/templates/nfd.conf.in"
PATH="$tmp/bin:$PATH" SLURM_JOB_ID=test SLURM_NNODES=1 NDNSF_SPEC110_TEST_MODE=1 NDNSF_SPEC110_READINESS_SETTLE_SECONDS=0.1 \
  "$supervisor" --process-map "$supervisor_scratch/process-map.json" --scratch "$supervisor_scratch" \
  --evidence "$tmp/supervisor-normal" --nfd-template "$template"
python3 - "$tmp/supervisor-normal/teardown.json" <<'PY'
import json,sys
value=json.load(open(sys.argv[1]));assert value['status']=='PASS' and value['survivors']==0 and value['exitCode']==0
PY
grep -q CANDIDATE_PROCESS_GRAPH_COMPLETED "$tmp/supervisor-normal/readiness-verdict.txt"

signal_scratch=$(mktemp -d /tmp/ndnsf-di-signal.XXXXXX)
cp "$supervisor_scratch/process-map.json" "$signal_scratch/process-map.json"
python3 - "$repo" "$signal_scratch/process-map.json" "$signal_scratch" <<'PY'
import json,sys
sys.path.insert(0,sys.argv[1]+'/packaging/ndnsf-di-container/lib');from allocation_topology import command_digest
path=sys.argv[2];root=sys.argv[3];value=json.load(open(path));old=value['nodes'][0]['nfdSocket'];new=root+'/nfd/0/nfd.sock';value['nodes'][0]['nfdSocket']=new
for process in value['processes']:
 process['nfdSocket']=new
 if process['kind']=='nfd':process['command']=['nfd','--config',root+'/nfd/0/nfd.conf'];process['commandDigest']=command_digest(process['command'])
json.dump(value,open(path,'w'))
PY
set +e
PATH="$tmp/bin:$PATH" SLURM_JOB_ID=test SLURM_NNODES=1 NDNSF_SPEC110_TEST_MODE=1 NDNSF_SPEC110_READINESS_SETTLE_SECONDS=5 \
  "$supervisor" --process-map "$signal_scratch/process-map.json" --scratch "$signal_scratch" \
  --evidence "$tmp/supervisor-signal" --nfd-template "$template" &
supervisor_pid=$!
sleep 0.5
kill -TERM "$supervisor_pid"
wait "$supervisor_pid"
signal_rc=$?
set -e
[[ $signal_rc -eq 143 ]]
python3 - "$tmp/supervisor-signal/teardown.json" <<'PY'
import json,sys
value=json.load(open(sys.argv[1]));assert value['status']=='FAIL' and value['survivors']==0 and value['exitCode']==143
PY
rm -rf "$signal_scratch"
printf 'NETWORK_SCRIPT_PASS\n'
