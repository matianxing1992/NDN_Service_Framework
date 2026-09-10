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
if [[ ${1:-} == --exclusive ]]; then
  echo "FAKE_SRUN_EXCLUSIVE_STEP_FORBIDDEN" >&2
  exit 99
fi
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
if [[ ${SPEC110_FAIL_WORKDIR:-0} == 1 && ${1:-} == test && ${2:-} == -d ]]; then
  exit 1
fi
if [[ ${SPEC110_FAIL_PORT_PROBE:-0} == 1 && ${1:-} == python3 && ${2:-} == -c ]]; then
  exit 1
fi
if [[ ${SPEC110_FAIL_IDENTITY_SYMLINK:-0} == 1 && ${1:-} == sh && ${2:-} == -c ]]; then
  exit 1
fi
if [[ ${SPEC110_FAIL_IDENTITY_SYMLINK:-0} == 1 && ${1:-} == test && ${2:-} == ! && ${3:-} == -L ]]; then
  exit 1
fi
if [[ ${SPEC110_FAIL_ADDRESS_PROBE:-0} == 1 && ${1:-} == test && ${2:-} == -r ]]; then
  exit 0
fi
if [[ ${SPEC110_FAIL_ADDRESS_PROBE:-0} == 1 && ${1:-} == python3 && ${2:-} == -c ]]; then
  exit 1
fi
if [[ ${SPEC110_FAIL_IDENTITY_SYMLINK:-0} == 1 && ${1:-} == test && ${2:-} == -r ]]; then
  exit 0
fi
exec "$@"
SH
cat >"$tmp/bin/nfdc" <<'SH'
#!/bin/sh
printf 'fake-nfdc %s\n' "$*"
SH
chmod 0755 "$tmp/bin/srun" "$tmp/bin/nfdc"

for script in \
  "$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-allocation-topology.sh" \
  "$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/configure-allocation-routes.sh" \
  "$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/probe-multinode-network.sh"; do
  if grep -q -- '--exclusive' "$script"; then
    echo "EXCLUSIVE_SLURM_STEP_FORBIDDEN:$script" >&2
    exit 99
  fi
done
python3 - "$repo/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-allocation-topology.sh" <<'PY'
import pathlib, sys
source = pathlib.Path(sys.argv[1]).read_text()
loop = source.index('for row in "${node_rows[@]}"; do')
deadline = source.index('deadline=$((SECONDS+30))', loop)
probe = source.index('while ((SECONDS < deadline))', deadline)
assert deadline < probe
assert 'deadline=$((SECONDS+30))' not in source[:loop]
stale = source.index('srun_node "$rank" rm -f "$socket"')
launch = source.index('setsid "${srun_step[@]}" "--relative=$rank"', stale)
alive = source.index('kill -0 "${nfd_steps[$rank]}"', launch)
assert stale < launch < alive
assert 'srun_node "$rank" test ! -L "$identity"' in source
assert 'SPEC110_NODE_ADDRESS_NOT_LOCAL' in source
PY
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
  --evidence "$tmp/supervisor-normal" --nfd-template "$template" --workdir "$tmp"
python3 - "$tmp/supervisor-normal/teardown.json" <<'PY'
import json,sys
value=json.load(open(sys.argv[1]));assert value['status']=='PASS' and value['survivors']==0 and value['exitCode']==0
PY
for launcher in nfd-0 controller user provider-0 provider-1 provider-2; do
  test -x "$supervisor_scratch/generated/$launcher.sh"
done
grep -q CANDIDATE_PROCESS_GRAPH_COMPLETED "$tmp/supervisor-normal/readiness-verdict.txt"

set +e
PATH="$tmp/bin:$PATH" SLURM_JOB_ID=test SLURM_NNODES=1 NDNSF_SPEC110_TEST_MODE=1 SPEC110_FAIL_WORKDIR=1 \
  "$supervisor" --process-map "$supervisor_scratch/process-map.json" --scratch "$supervisor_scratch" \
  --evidence "$tmp/supervisor-workdir-fail" --nfd-template "$template" --workdir "$tmp"
workdir_rc=$?
set -e
[[ $workdir_rc -eq 4 ]]
python3 - "$tmp/supervisor-workdir-fail/teardown.json" <<'PY'
import json,sys
value=json.load(open(sys.argv[1]));assert value['status']=='FAIL' and value['survivors']==0 and value['exitCode']==4
PY

set +e
PATH="$tmp/bin:$PATH" SLURM_JOB_ID=test SLURM_NNODES=1 NDNSF_SPEC110_TEST_MODE=1 \
  "$supervisor" --process-map "$supervisor_scratch/process-map.json" --scratch "$supervisor_scratch" \
  --evidence "$tmp/supervisor-invalid-workdir" --nfd-template "$template" --workdir relative-workdir
invalid_workdir_rc=$?
set -e
[[ $invalid_workdir_rc -eq 3 ]]
python3 - "$tmp/supervisor-invalid-workdir/teardown.json" <<'PY'
import json,sys
value=json.load(open(sys.argv[1]));assert value['status']=='FAIL' and value['survivors']==0 and value['exitCode']==3
PY

signal_scratch=$(mktemp -d /tmp/ndnsf-di-test-signal.XXXXXX)
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
  --evidence "$tmp/supervisor-signal" --nfd-template "$template" --workdir "$tmp" &
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

identity_scratch=$(mktemp -d /tmp/ndnsf-di-test-identity.XXXXXX)
cp "$supervisor_scratch/process-map.json" "$identity_scratch/process-map.json"
python3 - "$repo" "$identity_scratch/process-map.json" "$identity_scratch" <<'PY'
import json,sys
sys.path.insert(0,sys.argv[1]+'/packaging/ndnsf-di-container/lib')
from allocation_topology import command_digest
path,root=sys.argv[2:]
value=json.load(open(path)); socket=root+'/nfd/0/nfd.sock'
value['nodes'][0]['nfdSocket']=socket
for process in value['processes']:
    process['nfdSocket']=socket
    if process['kind']=='nfd':
        process['command']=['nfd','--config',root+'/nfd/0/nfd.conf']
        process['commandDigest']=command_digest(process['command'])
json.dump(value,open(path,'w'))
PY
set +e
PATH="$tmp/bin:$PATH" SLURM_JOB_ID=test SLURM_NNODES=1 NDNSF_SPEC110_TEST_MODE=0 \
  "$supervisor" --process-map "$identity_scratch/process-map.json" --scratch "$identity_scratch" \
  --evidence "$tmp/supervisor-identity-fail" --nfd-template "$template" --workdir "$tmp"
identity_rc=$?
set -e
[[ $identity_rc -eq 4 ]]
python3 - "$tmp/supervisor-identity-fail/teardown.json" <<'PY'
import json,sys
value=json.load(open(sys.argv[1]))
assert value['status']=='FAIL' and value['exitCode']==4 and value['survivors']==0
PY
[[ ! -e "$identity_scratch/log/nfd-0.log" ]]
rm -rf "$identity_scratch"

set +e
scratch_job_scratch=$(mktemp -d /tmp/ndnsf-di-test-mismatch.XXXXXX)
cp "$supervisor_scratch/process-map.json" "$scratch_job_scratch/process-map.json"
PATH="$tmp/bin:$PATH" SLURM_JOB_ID=999 SLURM_NNODES=1 NDNSF_SPEC110_TEST_MODE=0 \
  "$supervisor" --process-map "$scratch_job_scratch/process-map.json" --scratch "$scratch_job_scratch" \
  --evidence "$tmp/supervisor-scratch-job-fail" --nfd-template "$template" --workdir "$tmp"
scratch_job_rc=$?
set -e
[[ $scratch_job_rc -eq 3 ]]
python3 - "$tmp/supervisor-scratch-job-fail/teardown.json" <<'PY'
import json,sys
value=json.load(open(sys.argv[1]))
assert value['status']=='FAIL' and value['exitCode']==3 and value['survivors']==0
PY
[[ ! -e "$scratch_job_scratch/log/nfd-0.log" ]]
rm -rf "$scratch_job_scratch"

set +e
address_scratch=$(mktemp -d /tmp/ndnsf-di-test-address.XXXXXX)
cp "$supervisor_scratch/process-map.json" "$address_scratch/process-map.json"
python3 - "$repo" "$address_scratch/process-map.json" "$address_scratch" <<'PY'
import json,sys
sys.path.insert(0,sys.argv[1]+'/packaging/ndnsf-di-container/lib')
from allocation_topology import command_digest
path,root=sys.argv[2:]
value=json.load(open(path)); socket=root+'/nfd/0/nfd.sock'
value['nodes'][0]['nfdSocket']=socket
for process in value['processes']:
    process['nfdSocket']=socket
    if process['kind']=='nfd':
        process['command']=['nfd','--config',root+'/nfd/0/nfd.conf']
        process['commandDigest']=command_digest(process['command'])
json.dump(value,open(path,'w'))
PY
PATH="$tmp/bin:$PATH" SLURM_JOB_ID=test SLURM_NNODES=1 NDNSF_SPEC110_TEST_MODE=0 SPEC110_FAIL_ADDRESS_PROBE=1 \
  "$supervisor" --process-map "$address_scratch/process-map.json" --scratch "$address_scratch" \
  --evidence "$tmp/supervisor-address-fail" --nfd-template "$template" --workdir "$tmp"
address_rc=$?
set -e
[[ $address_rc -eq 4 ]]
python3 - "$tmp/supervisor-address-fail/teardown.json" <<'PY'
import json,sys
value=json.load(open(sys.argv[1]))
assert value['status']=='FAIL' and value['exitCode']==4 and value['survivors']==0
PY
[[ ! -e "$address_scratch/log/nfd-0.log" ]]
rm -rf "$address_scratch"

port_scratch=$(mktemp -d /tmp/ndnsf-di-test-port.XXXXXX)
cp "$supervisor_scratch/process-map.json" "$port_scratch/process-map.json"
python3 - "$repo" "$port_scratch/process-map.json" "$port_scratch" <<'PY'
import json,sys
sys.path.insert(0,sys.argv[1]+'/packaging/ndnsf-di-container/lib')
from allocation_topology import command_digest
path,root=sys.argv[2:]
value=json.load(open(path)); socket=root+'/nfd/0/nfd.sock'
value['nodes'][0]['nfdSocket']=socket
for process in value['processes']:
    process['nfdSocket']=socket
    if process['kind']=='nfd':
        process['command']=['nfd','--config',root+'/nfd/0/nfd.conf']
        process['commandDigest']=command_digest(process['command'])
json.dump(value,open(path,'w'))
PY
set +e
PATH="$tmp/bin:$PATH" SLURM_JOB_ID=test SLURM_NNODES=1 NDNSF_SPEC110_TEST_MODE=1 SPEC110_FAIL_PORT_PROBE=1 \
  "$supervisor" --process-map "$port_scratch/process-map.json" --scratch "$port_scratch" \
  --evidence "$tmp/supervisor-port-fail" --nfd-template "$template" --workdir "$tmp"
port_rc=$?
set -e
[[ $port_rc -eq 4 ]]
python3 - "$tmp/supervisor-port-fail/teardown.json" <<'PY'
import json,sys
value=json.load(open(sys.argv[1]))
assert value['status']=='FAIL' and value['exitCode']==4 and value['survivors']==0
PY
[[ ! -e "$port_scratch/log/nfd-0.log" ]]
rm -rf "$port_scratch"

symlink_scratch=$(mktemp -d /tmp/ndnsf-di-test-symlink.XXXXXX)
cp "$supervisor_scratch/process-map.json" "$symlink_scratch/process-map.json"
python3 - "$repo" "$symlink_scratch/process-map.json" "$symlink_scratch" <<'PY'
import json,sys
sys.path.insert(0,sys.argv[1]+'/packaging/ndnsf-di-container/lib')
from allocation_topology import command_digest
path,root=sys.argv[2:]
value=json.load(open(path)); socket=root+'/nfd/0/nfd.sock'
value['nodes'][0]['nfdSocket']=socket
for process in value['processes']:
    process['nfdSocket']=socket
    if process['kind']=='nfd':
        process['command']=['nfd','--config',root+'/nfd/0/nfd.conf']
        process['commandDigest']=command_digest(process['command'])
json.dump(value,open(path,'w'))
PY
set +e
PATH="$tmp/bin:$PATH" SLURM_JOB_ID=test SLURM_NNODES=1 NDNSF_SPEC110_TEST_MODE=0 SPEC110_FAIL_IDENTITY_SYMLINK=1 \
  "$supervisor" --process-map "$symlink_scratch/process-map.json" --scratch "$symlink_scratch" \
  --evidence "$tmp/supervisor-symlink-fail" --nfd-template "$template" --workdir "$tmp"
symlink_rc=$?
set -e
[[ $symlink_rc -eq 4 ]]
python3 - "$tmp/supervisor-symlink-fail/teardown.json" <<'PY'
import json,sys
value=json.load(open(sys.argv[1]))
assert value['status']=='FAIL' and value['exitCode']==4 and value['survivors']==0
PY
[[ ! -e "$symlink_scratch/log/nfd-0.log" ]]
rm -rf "$symlink_scratch"
printf 'NETWORK_SCRIPT_PASS\n'
