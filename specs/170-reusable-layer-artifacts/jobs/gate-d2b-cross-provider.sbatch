#!/usr/bin/env bash
# Spec170 T024/T033: two Providers, one GPU on each allocated node.
#SBATCH --job-name=spec170-d2b-cross-provider
#SBATCH --partition=bigTiger
#SBATCH --nodes=2
#SBATCH --ntasks=2
#SBATCH --ntasks-per-node=1
#SBATCH --cpus-per-task=8
#SBATCH --mem=32G
#SBATCH --gres=gpu:1
#SBATCH --time=00:45:00

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
test "${SPEC170_GATE:-d2b-cross-provider}" = d2b-cross-provider
case "$SPEC170_WORKLOAD" in /*) ;; *) echo 'SPEC170_WORKLOAD_MUST_BE_CONTAINER_ABSOLUTE' >&2; exit 2 ;; esac
# The launcher is written on the host and then executed inside the contained
# namespace. Rewrite a release-host path to the corresponding /release bind.
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
run_container="$SPEC170_PROJECT_ROOT/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/run-container.sh"
preflight="$SPEC170_PROJECT_ROOT/packaging/ndnsf-di-container/adapters/slurm-apptainer/scripts/preflight-compute.sh"
test -x "$run_container"
test -x "$preflight"
mapfile -t d2b_hosts < <(scontrol show hostnames "$SLURM_JOB_NODELIST")
test "${#d2b_hosts[@]}" -eq 2 || {
  echo "SPEC170_D2B_NODELIST_FAIL count=${#d2b_hosts[@]}" >&2
  exit 2
}
d2b_run_id="${NDNSF_D2B_RUN_ID:-spec170-d2b-${SLURM_JOB_ID}}"
d2b_case="${SPEC170_D2B_CASE:-positive}"
case "$d2b_case" in
  positive|peer-mismatch|replay|partial) ;;
  *) echo "SPEC170_D2B_CASE_INVALID case=$d2b_case" >&2; exit 2 ;;
esac
# /tmp is node-local; initialize the validation directory in every task before
# the shared preflight and wrapper inspect it.
srun --kill-on-bad-exit=1 --label --nodes=2 --ntasks=2 --ntasks-per-node=1 --export=ALL \
  bash -c 'set -euo pipefail; mkdir -p "$1/evidence"; echo "SPEC170_D2B_SCRATCH_PASS host=$(hostname)"' _ "$scratch"
srun --kill-on-bad-exit=1 --label --ntasks=2 --ntasks-per-node=1 --export=ALL \
  "$preflight" --scratch "$scratch" --gpu-type allocated --gpu-count 1
# Every node must finish staging and hashing the exact SIF before either peer
# starts NFD or attempts to create the cross-Provider face.  Without this
# barrier, a fast node can time out while a slower node is still copying 4+ GB.
srun --kill-on-bad-exit=1 --label --nodes=2 --ntasks=2 --ntasks-per-node=1 --export=ALL \
  "$run_container" --sif "$SPEC170_RUNTIME_SIF" \
  --sif-sha256 "$SPEC170_RUNTIME_SIF_SHA256" \
  --build-record "$SPEC170_RUNTIME_BUILD_RECORD" --gpu-count 1 \
  --project "$SPEC170_PROJECT_ROOT" --scratch "$scratch" \
  --identity "$SPEC170_IDENTITY_ROOT" -- "/bin/true"
echo "SPEC170_D2B_SIF_STAGE_BARRIER_PASS jobId=${SLURM_JOB_ID}"
echo "SPEC170_D2B_WORKLOAD_BIND_PASS host=$SPEC170_WORKLOAD container=$container_workload"
srun --kill-on-bad-exit=1 --label --ntasks=2 --ntasks-per-node=1 --export=ALL \
  bash -c '
    set -euo pipefail
    scratch="$1"
    run_id="$2"
    d2b_case="$3"
    workload="$4"
    host0="$5"
    host1="$6"
    if [[ "$SLURM_PROCID" == 0 ]]; then
      peer="$host1"
    elif [[ "$SLURM_PROCID" == 1 ]]; then
      peer="$host0"
    else
      echo "SPEC170_D2B_RANK_FAIL rank=$SLURM_PROCID" >&2
      exit 2
    fi
    launcher="$scratch/launch-d2b.sh"
    {
      printf "%s\\n" "#!/bin/sh"
      printf "export NDNSF_D2B_RANK=%q\\n" "$SLURM_PROCID"
      printf "export NDNSF_D2B_RUN_ID=%q\\n" "$run_id"
      printf "export NDNSF_D2B_CASE=%q\\n" "$d2b_case"
      printf "export NDNSF_D2B_PEER_HOST=%q\\n" "$peer"
      printf "export SPEC170_D2B_DEVICE=%q\\n" "${SPEC170_D2B_DEVICE:-cuda:0}"
      printf "exec %q\\n" "$workload"
    } >"$launcher"
    chmod 0755 "$launcher"
    printf "rank=%s\\nrunId=%s\\npeerHost=%s\\n" "$SLURM_PROCID" "$run_id" "$peer" >"$scratch/evidence/d2b-launch-$SLURM_PROCID.txt"
    exec "$7" --sif "$8" --sif-sha256 "$9" --build-record "${10}" --gpu-count 1 \
      --project "${11}" --scratch "$scratch" --identity "${12}" -- "/scratch/launch-d2b.sh"
  ' _ "$scratch" "$d2b_run_id" "$d2b_case" "$container_workload" \
  "${d2b_hosts[0]}" "${d2b_hosts[1]}" "$run_container" "$SPEC170_RUNTIME_SIF" \
  "$SPEC170_RUNTIME_SIF_SHA256" "$SPEC170_RUNTIME_BUILD_RECORD" \
  "$SPEC170_PROJECT_ROOT" "$SPEC170_IDENTITY_ROOT"
echo "SPEC170_D2B_TWO_PROVIDER_GPU_RUNTIME_PASS jobId=${SLURM_JOB_ID} case=${d2b_case}"
