#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=8G
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=00:05:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec160/qwen-live/provider-ctor-py38-diag-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec160/qwen-live/provider-ctor-py38-diag-%j.err
set -Eeuo pipefail
: "${SPEC160_LIVE_SOURCE_ROOT:?}"
: "${SPEC160_ARTIFACT_DIR:?}"
SIF=/project/tma1/ndnsf-di/releases/spec159-3ac97b8792ff302d/runtime.sif
WORK=${SLURM_TMPDIR:-/tmp/${USER}}/spec160-provider-py38-${SLURM_JOB_ID}
mkdir -p "$WORK/run" "$WORK/home" "$WORK/log"
sed -e 's|@@NFD_SOCKET@@|/scratch/run/nfd.sock|g' \
    -e 's|@@PORT@@|33386|g' \
    "$SPEC160_LIVE_SOURCE_ROOT/nfd.conf.in" > "$WORK/nfd.conf"
apptainer exec --nv --cleanenv --containall \
  --home "$WORK/home:/home/${USER}" \
  --bind "$WORK:/scratch:rw" \
  --bind "$SPEC160_LIVE_SOURCE_ROOT:/source:ro" \
  --bind "$SPEC160_ARTIFACT_DIR:$SPEC160_ARTIFACT_DIR:ro" \
  --env LD_LIBRARY_PATH=/source/native-libs:/opt/ndn-base/lib:/opt/ndnsf-app/lib:/usr/local/lib:/usr/lib:/lib:/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu \
  --env PYTHONPATH=/source/pythonWrapper:/source/repoPythonWrapper:/source:/source/llm_pipeline \
  --env NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock \
  --env SPEC160_ARTIFACT_DIR="$SPEC160_ARTIFACT_DIR" \
  "$SIF" bash -s <<'INNER'
set -Eeuo pipefail
nfd --config /scratch/nfd.conf > /scratch/log/nfd.log 2>&1 &
nfd_pid=$!
cleanup(){ kill "$nfd_pid" 2>/dev/null || true; wait "$nfd_pid" 2>/dev/null || true; }
trap cleanup EXIT
for _ in $(seq 1 200); do
  test -S /scratch/run/nfd.sock && timeout 5s nfdc status >/dev/null 2>&1 && break
  sleep 0.1
done
timeout 5s nfdc status >/dev/null
python3 - <<'PY'
from ndnsf_distributed_inference.policy import write_policy_bundle
import os
dep = write_policy_bundle(os.environ['SPEC160_ARTIFACT_DIR'] + '/policy.yaml', '/scratch/generated-policy')
print('TRACE generated', dep.trust_schema, dep.policy_file, flush=True)
PY
python3.8 -X faulthandler - <<'PY'
import faulthandler, sys
faulthandler.enable(file=sys.stderr)
print('TRACE version', sys.version, flush=True)
for mod in ['ndnsf._ndnsf', 'ndnsf.service']:
    try:
        m = __import__(mod, fromlist=['*'])
        print('TRACE import', mod, 'OK', getattr(m, '__file__', None), flush=True)
    except Exception as exc:
        print('TRACE import', mod, 'FAIL', type(exc).__name__, repr(exc), flush=True)
from ndnsf.service import ServiceProvider
provider = ServiceProvider(
    provider_id='',
    group='/NDNSF-DistributeInference/example/group',
    controller='/NDNSF-DistributeInference/example/controller',
    provider_prefix='/NDNSF-DistributeInference/example/provider',
    trust_schema='/scratch/generated-policy/trust-schema.conf',
    handler_threads=4,
    ack_threads=2,
    serve_certificates=True,
)
print('TRACE ctor-ok', flush=True)
provider.stop()
print('TRACE stop-ok', flush=True)
PY
INNER
