#!/bin/bash
#SBATCH --partition=bigTiger
#SBATCH --account=devs
#SBATCH --qos=normal
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=32G
#SBATCH --gres=gpu:rtx_5000:1
#SBATCH --time=00:10:00
#SBATCH --output=/project/tma1/ndnsf-di/evidence/spec160/qwen-live/provider-start-trace-%j.out
#SBATCH --error=/project/tma1/ndnsf-di/evidence/spec160/qwen-live/provider-start-trace-%j.err
set -Eeuo pipefail
: "${SPEC160_LIVE_SOURCE_ROOT:?}"
: "${SPEC160_ARTIFACT_DIR:?}"
SIF=/project/tma1/ndnsf-di/releases/spec159-3ac97b8792ff302d/runtime.sif
WORK=${SLURM_TMPDIR:-/tmp/${USER}}/spec160-provider-trace-${SLURM_JOB_ID}
mkdir -p "$WORK/run" "$WORK/home" "$WORK/log"
sed -e 's|@@NFD_SOCKET@@|/scratch/run/nfd.sock|g' \
    -e 's|@@PORT@@|33384|g' \
    "$SPEC160_LIVE_SOURCE_ROOT/nfd.conf.in" > "$WORK/nfd.conf"

apptainer exec --nv --cleanenv --containall \
  --home "$WORK/home:/home/${USER}" \
  --bind "$WORK:/scratch:rw" \
  --bind "$SPEC160_LIVE_SOURCE_ROOT:/source:ro" \
  --bind "$SPEC160_ARTIFACT_DIR:$SPEC160_ARTIFACT_DIR:ro" \
  --env LD_LIBRARY_PATH=/source/native-libs:/opt/ndn-base/lib:/opt/ndnsf-app/lib:/usr/local/lib:/usr/lib:/lib:/usr/lib/x86_64-linux-gnu:/lib/x86_64-linux-gnu \
  --env PYTHONPATH=/source/vendor-site:/source/pythonWrapper:/source/repoPythonWrapper:/source:/source/llm_pipeline \
  --env NDN_CLIENT_TRANSPORT=unix:///scratch/run/nfd.sock \
  --env SPEC160_POLICY="$SPEC160_ARTIFACT_DIR/policy.yaml" \
  --env MALLOC_CHECK_=3 \
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

run_case() {
  local label=$1 provider_id=$2 role=$3
  echo "==== CASE $label provider_id=${provider_id:-root} role=$role"
  timeout 120s python3 -X faulthandler - <<PY
import faulthandler, sys, time
faulthandler.enable(file=sys.stderr)
faulthandler.dump_traceback_later(30, repeat=True, file=sys.stderr)
from ndnsf_distributed_inference.app_sdk.provider import APPProvider
from provider import SERVICE, _selected_roles, _preload_qwen_stage_models, handle_qwen_transformer_stage
print("TRACE import-ok", flush=True)
provider = APPProvider.from_config(
    "$SPEC160_POLICY",
    generated_policy_dir="/scratch/generated-$label",
    provider_id="$provider_id",
    group="/NDNSF-DistributeInference/example/group",
    handler_workers=2,
)
print("TRACE from_config-ok", flush=True)
roles = _selected_roles("$role", provider)
print("TRACE selected_roles", sorted(roles), flush=True)
models = _preload_qwen_stage_models(provider, roles, device="cuda:0", require_cuda=True)
print("TRACE preload-ok", sorted(models.keys()), flush=True)
handler = lambda ctx: handle_qwen_transformer_stage(ctx, stages=3, model_cache=models, compute_delay_ms=0)
provider.serve_service(service=SERVICE, roles="$role", handler=handler, backends=["transformers"], has_model=True, can_provision=False)
print("TRACE serve_service-ok", flush=True)
provider.stop()
print("TRACE stop-ok", flush=True)
PY
  echo "==== CASE_RC $label $?"
}

run_case stage0 "" "/LLM/Pipeline/Stage/0" || true
run_case stage1 "1" "/LLM/Pipeline/Stage/1" || true
run_case stage2 "2" "/LLM/Pipeline/Stage/2" || true
INNER
