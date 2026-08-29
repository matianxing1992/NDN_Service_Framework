#!/bin/bash
set -euo pipefail

usage() {
  echo "usage: $0 --scratch DIR --evidence DIR --nfd-config FILE --controller-args FILE --provider-args-dir DIR --user-args FILE" >&2
  exit 2
}

scratch='' evidence='' nfd_config='' controller_args='' provider_args_dir='' user_args=''
while (($#)); do
  case "$1" in
    --scratch) scratch=$2; shift 2 ;;
    --evidence) evidence=$2; shift 2 ;;
    --nfd-config) nfd_config=$2; shift 2 ;;
    --controller-args) controller_args=$2; shift 2 ;;
    --provider-args-dir) provider_args_dir=$2; shift 2 ;;
    --user-args) user_args=$2; shift 2 ;;
    *) usage ;;
  esac
done
[[ -n ${SLURM_JOB_ID:-} ]] || { echo NDNSF_QWEN_REQUIRES_SLURM >&2; exit 3; }
for value in "$scratch" "$evidence" "$nfd_config" "$controller_args" "$provider_args_dir" "$user_args"; do
  [[ -n $value ]] || usage
done
[[ -f $nfd_config && -f $controller_args && -d $provider_args_dir && -f $user_args ]] || {
  echo NDNSF_QWEN_INPUT_MISSING >&2; exit 4;
}
mkdir -p "$scratch/run" "$scratch/log" "$evidence"
chmod 700 "$scratch" "$scratch/run"
export NDN_CLIENT_TRANSPORT="unix://$scratch/run/nfd.sock"
export NDNSF_DI_ORT_PROFILE_PREFIX="$scratch/ort-profile"
if command -v nvidia-smi >/dev/null 2>&1; then
  export NDNSF_DI_GPU_UUID
  NDNSF_DI_GPU_UUID=$(nvidia-smi --query-gpu=uuid --format=csv,noheader | paste -sd, -)
fi

pids=()
controller_pid=
provider_pids=()
provider_logs=()
finish() {
  rc=$?
  trap - EXIT INT TERM
  for pid in "${pids[@]:-}"; do kill "$pid" 2>/dev/null || true; done
  wait "${pids[@]:-}" 2>/dev/null || true
  cp -a "$scratch/log/." "$evidence/" 2>/dev/null || true
  find "$scratch" -maxdepth 1 -name 'ort-profile*.json' -exec cp -p {} "$evidence/" \; 2>/dev/null || true
  printf '{"schemaVersion":"1.0","slurmJobId":"%s","exitCode":%d,"status":"%s"}\n' \
    "$SLURM_JOB_ID" "$rc" "$([[ $rc -eq 0 ]] && echo COMPLETED || echo FAILED)" \
    >"$evidence/orchestration-terminal.json"
  exit "$rc"
}
trap finish EXIT INT TERM

nfd --config "$nfd_config" >"$scratch/log/nfd.log" 2>&1 & pids+=("$!")
ready=0
for _ in $(seq 1 100); do
  if [[ -S $scratch/run/nfd.sock ]] && nfdc status >/dev/null 2>&1; then ready=1; break; fi
  sleep 0.1
done
[[ $ready -eq 1 ]] || { echo NFD_READINESS_TIMEOUT >&2; exit 5; }
nfdc status report >"$scratch/log/nfd-status.txt"

run_args_file() {
  local file=$1 log=$2
  local -a args=()
  mapfile -t args <"$file"
  ((${#args[@]})) || { echo "EMPTY_ARGUMENT_FILE:$file" >&2; return 6; }
  "${args[@]}" >"$log" 2>&1 &
  pids+=("$!")
}

run_args_file "$controller_args" "$scratch/log/controller.log"
controller_pid=${pids[-1]}
shopt -s nullglob
provider_arg_files=("$provider_args_dir"/*.args)
((${#provider_arg_files[@]})) || { echo PROVIDER_ARGUMENTS_MISSING >&2; exit 6; }
for file in "${provider_arg_files[@]}"; do
  log="$scratch/log/$(basename "${file%.args}").log"
  run_args_file "$file" "$log"
  provider_pids+=("${pids[-1]}")
  provider_logs+=("$log")
done

controller_ready() {
  kill -0 "$controller_pid" 2>/dev/null &&
    grep -Eq 'NDNSF_DI_CONTROLLER_READY|ServiceController started\.\.\.|controller ready' \
      "$scratch/log/controller.log" 2>/dev/null
}

all_providers_ready() {
  local index
  for index in "${!provider_logs[@]}"; do
    kill -0 "${provider_pids[$index]}" 2>/dev/null || return 1
    grep -q 'NDNSF_DI_NATIVE_PROVIDER_READY' "${provider_logs[$index]}" 2>/dev/null || return 1
  done
}

for _ in $(seq 1 300); do
  if controller_ready && all_providers_ready; then
    break
  fi
  sleep 0.1
done
controller_ready || {
  echo CONTROLLER_READINESS_TIMEOUT >&2; exit 7;
}
all_providers_ready || {
  echo PROVIDER_READINESS_TIMEOUT >&2; exit 7;
}

mapfile -t user_command <"$user_args"
((${#user_command[@]})) || { echo USER_ARGUMENTS_EMPTY >&2; exit 6; }
"${user_command[@]}" >"$scratch/log/user.log" 2>&1
