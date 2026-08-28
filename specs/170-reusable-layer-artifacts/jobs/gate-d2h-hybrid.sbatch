#!/usr/bin/env bash
# Spec170 T024/T034: two Providers, one GPU each, heterogeneous hybrid plan.
#SBATCH --job-name=spec170-d2h-hybrid
#SBATCH --partition=bigTiger
#SBATCH --nodes=2
#SBATCH --ntasks=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=12
#SBATCH --mem=48G
#SBATCH --gres=gpu:1
#SBATCH --time=01:00:00

set -euo pipefail
unset APPTAINERENV_HOME SINGULARITYENV_HOME
: "${SPEC170_PROJECT_ROOT:?}"
: "${SPEC170_RUNTIME_SIF:?}"
: "${SPEC170_RUNTIME_SIF_SHA256:?}"
: "${SPEC170_RUNTIME_BUILD_RECORD:?}"
if [[ ! "$SPEC170_RUNTIME_SIF_SHA256" =~ ^sha256:[0-9a-f]{64}$ ]]; then
  echo 'SPEC170_RUNTIME_SIF_SHA256_MUST_BE_PREFIXED' >&2
  exit 2
fi
: "${SPEC170_IDENTITY_ROOT:?}"
: "${SPEC170_WORKLOAD:?}"
: "${SPEC170_HYBRID_MAPPING:?}"
test "${SPEC170_GATE:-d2h-hybrid}" = d2h-hybrid
case "$SPEC170_WORKLOAD" in /*) ;; *) echo 'SPEC170_WORKLOAD_MUST_BE_CONTAINER_ABSOLUTE' >&2; exit 2 ;; esac
container_workload="$SPEC170_WORKLOAD"
case "$SPEC170_WORKLOAD" in
  "$SPEC170_PROJECT_ROOT"/releases/*)
    container_workload="/release/${SPEC170_WORKLOAD#"$SPEC170_PROJECT_ROOT/releases/"}"
    ;;
  /release/*|/scratch/*|/bin/*)
    ;;
  *)
    echo "SPEC170_WORKLOAD_BIND_PATH_FAIL host=$SPEC170_WORKLOAD" >&2
    exit 2
    ;;
esac
test -f "$SPEC170_RUNTIME_SIF"
scratch="/tmp/ndnsf-di-${SLURM_JOB_ID}"
mkdir -p "$scratch/evidence"
trap 'rm -rf "$scratch"' EXIT
preflight="$SPEC170_PROJECT_ROOT/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/preflight-compute.sh"
run_container="$SPEC170_PROJECT_ROOT/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-container.sh"
test -x "$preflight"
test -x "$run_container"
mapfile -t d2h_hosts < <(scontrol show hostnames "$SLURM_JOB_NODELIST")
test "${#d2h_hosts[@]}" -eq 2 || {
  echo "SPEC170_D2H_NODELIST_FAIL count=${#d2h_hosts[@]}" >&2
  exit 2
}
d2h_run_id="${NDNSF_D2H_RUN_ID:-spec170-d2h-${SLURM_JOB_ID}-${SPEC170_HYBRID_MAPPING}}"
srun --kill-on-bad-exit=1 --label --nodes=2 --ntasks=2 --ntasks-per-node=1 --export=ALL \
  bash -c 'set -euo pipefail; mkdir -p "$2/evidence"; printf "%s\\n" "$1" > "$2/evidence/hybrid-mapping.txt"; echo "SPEC170_D2H_SCRATCH_PASS host=$(hostname)"' _ "$SPEC170_HYBRID_MAPPING" "$scratch"
srun --kill-on-bad-exit=1 --label --nodes=2 --ntasks=2 --ntasks-per-node=1 --export=ALL \
  "$preflight" --scratch "$scratch" --gpu-type allocated --gpu-count 1
# Stage and hash the exact SIF on both nodes before either Provider starts.
# Cross-node peer setup must never race an asymmetric node-local SIF copy.
srun --kill-on-bad-exit=1 --label --nodes=2 --ntasks=2 --ntasks-per-node=1 --export=ALL \
  "$run_container" --sif "$SPEC170_RUNTIME_SIF" \
  --sif-sha256 "$SPEC170_RUNTIME_SIF_SHA256" \
  --build-record "$SPEC170_RUNTIME_BUILD_RECORD" --gpu-count 1 \
  --project "$SPEC170_PROJECT_ROOT" --scratch "$scratch" \
  --identity "$SPEC170_IDENTITY_ROOT" -- "/bin/true"
echo "SPEC170_D2H_SIF_STAGE_BARRIER_PASS jobId=${SLURM_JOB_ID} mapping=${SPEC170_HYBRID_MAPPING}"
echo "SPEC170_D2H_WORKLOAD_BIND_PASS host=$SPEC170_WORKLOAD container=$container_workload"
srun --kill-on-bad-exit=1 --label --nodes=2 --ntasks=2 --ntasks-per-node=1 --export=ALL \
  bash -c '
    set -euo pipefail
    scratch="$1"
    run_id="$2"
    mapping="$3"
    workload="$4"
    host0="$5"
    host1="$6"
    if [[ "$SLURM_PROCID" == 0 ]]; then
      peer="$host1"
    elif [[ "$SLURM_PROCID" == 1 ]]; then
      peer="$host0"
    else
      echo "SPEC170_D2H_RANK_FAIL rank=$SLURM_PROCID" >&2
      exit 2
    fi
    launcher="$scratch/launch-d2h.sh"
    {
      printf "%s\\n" "#!/bin/sh"
      printf "export NDNSF_D2H_RANK=%q\\n" "$SLURM_PROCID"
      printf "export NDNSF_D2H_RUN_ID=%q\\n" "$run_id"
      printf "export NDNSF_D2H_PEER_HOST=%q\\n" "$peer"
      printf "export SPEC170_D2H_MAPPING=%q\\n" "$mapping"
      printf "export SPEC170_D2H_DEVICE=%q\\n" "${SPEC170_D2H_DEVICE:-cuda:0}"
      printf "exec %q\\n" "$workload"
    } >"$launcher"
    chmod 0755 "$launcher"
    printf "rank=%s\\nrunId=%s\\npeerHost=%s\\nmapping=%s\\n" "$SLURM_PROCID" "$run_id" "$peer" "$mapping" >"$scratch/evidence/d2h-launch-$SLURM_PROCID.txt"
    exec "$7" --sif "$8" --sif-sha256 "$9" --build-record "${10}" --gpu-count 1 \
      --project "${11}" --scratch "$scratch" --identity "${12}" -- "/scratch/launch-d2h.sh"
  ' _ "$scratch" "$d2h_run_id" "$SPEC170_HYBRID_MAPPING" "$container_workload" \
  "${d2h_hosts[0]}" "${d2h_hosts[1]}" "$run_container" "$SPEC170_RUNTIME_SIF" \
  "$SPEC170_RUNTIME_SIF_SHA256" "$SPEC170_RUNTIME_BUILD_RECORD" \
  "$SPEC170_PROJECT_ROOT" "$SPEC170_IDENTITY_ROOT"
echo "SPEC170_D2H_HYBRID_PASS jobId=${SLURM_JOB_ID} mapping=${SPEC170_HYBRID_MAPPING}"
