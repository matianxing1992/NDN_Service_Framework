#!/usr/bin/env bash
# Diagnostic only: run the immutable fc4e007f D1 workload with assignment
# decoding logs enabled.  This does not alter the SIF or the candidate bundle.
#SBATCH --job-name=spec170-fc4e-trace
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --gres=gpu:1
#SBATCH --exclude=itiger07
#SBATCH --time=00:15:00
#SBATCH --output=/project/tma1/ndnsf-di/campaigns/spec110/spec170-fc4e007f-d1-wire-trace/slurm-%j.out

set -euo pipefail
unset APPTAINERENV_HOME SINGULARITYENV_HOME
: "${SPEC170_PROJECT_ROOT:?}"
: "${SPEC170_TOOL_ROOT:?}"
: "${SPEC170_RUNTIME_SIF:?}"
: "${SPEC170_RUNTIME_SIF_SHA256:?}"
: "${SPEC170_IDENTITY_ROOT:?}"
: "${SPEC170_WORKLOAD:?}"
test "${SPEC170_GATE:-d1-wire-trace}" = d1-wire-trace
test "$(sha256sum "$SPEC170_RUNTIME_SIF" | cut -d' ' -f1)" = "${SPEC170_RUNTIME_SIF_SHA256#sha256:}"
scratch="/tmp/ndnsf-di-${SLURM_JOB_ID}"
mkdir -p "$scratch"
trap 'rm -rf "$scratch"' EXIT
cat > "$scratch/spec170-wire-trace.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
export NDNSF_COLLAB_ASSIGNMENT_FETCH_TRACE=1
export NDNSF_RUNTIME_TIMING=1
export NDN_LOG='ndn_service_framework.*=TRACE'
exec /release/spec170-runtime-fc4e007f-gpu-20260816-r1/network-bundle-d1-cuda-current/spec170-d1-current-sif-workload.sh
EOF
chmod 0755 "$scratch/spec170-wire-trace.sh"
"$SPEC170_TOOL_ROOT/preflight-compute.sh" \
  --scratch "$scratch" --gpu-type allocated --gpu-count 1
"$SPEC170_TOOL_ROOT/run-container.sh" \
  --sif "$SPEC170_RUNTIME_SIF" --sif-sha256 "$SPEC170_RUNTIME_SIF_SHA256" --gpu-count 1 \
  --project "$SPEC170_PROJECT_ROOT" --scratch "$scratch" --identity "$SPEC170_IDENTITY_ROOT" -- \
  /scratch/spec170-wire-trace.sh
echo "SPEC170_FC4E007F_D1_WIRE_TRACE_PASS jobId=${SLURM_JOB_ID}"
