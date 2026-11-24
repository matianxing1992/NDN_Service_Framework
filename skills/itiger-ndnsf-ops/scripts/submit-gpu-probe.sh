#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
source "${script_dir}/tiger-ssh.sh"
host="${ITIGER_SSH_HOST:-itiger}"
gpu="rtx_5000"
minutes=5
wait_for_job=false
image="docker://nvidia/cuda:12.4.1-base-ubuntu22.04"

usage()
{
  echo "Usage: $0 [--host ALIAS|USER@ALIAS] [--gpu GRES] [--minutes 1-60] [--image docker://URI] [--wait]"
}

while (($#)); do
  case "$1" in
    --host) host="${2:?missing host}"; shift 2 ;;
    --gpu) gpu="${2:?missing GPU GRES}"; shift 2 ;;
    --minutes) minutes="${2:?missing minutes}"; shift 2 ;;
    --image) image="${2:?missing image}"; shift 2 ;;
    --wait) wait_for_job=true; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

target="$(tiger_ssh_target "$host")"
[[ "${gpu}" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "Invalid GPU GRES" >&2; exit 2; }
[[ "${minutes}" =~ ^[0-9]+$ ]] && ((minutes >= 1 && minutes <= 60)) || { echo "Minutes must be 1-60" >&2; exit 2; }
[[ "${image}" =~ ^docker://[A-Za-z0-9._/@:-]+$ ]] || { echo "Image must be a simple docker:// URI" >&2; exit 2; }

printf -v time_limit '%02d:%02d:00' "$((minutes / 60))" "$((minutes % 60))"

remote_user="$(ssh "${TIGER_SSH_OPTIONS[@]}" "$target" id -un)"
[[ "$remote_user" == "${target%@*}" ]] || { echo "Unexpected remote user" >&2; exit 1; }

stamp="$(date -u +%Y%m%dT%H%M%SZ)"
probe_dir="/project/${remote_user}/ndnsf-di/probes/${gpu}-${stamp}"
remote_script="${probe_dir}/probe.sbatch"

ssh "${TIGER_SSH_OPTIONS[@]}" "$target" "mkdir -p '${probe_dir}' && chmod 700 '/project/${remote_user}/ndnsf-di' '/project/${remote_user}/ndnsf-di/probes' '${probe_dir}'"
scp -q "${TIGER_SSH_OPTIONS[@]}" "${script_dir}/itiger-gpu-probe.sbatch" "${target}:${remote_script}"

job_id="$(ssh "${TIGER_SSH_OPTIONS[@]}" "$target" sbatch --parsable \
  --partition=bigTiger \
  --gres="gpu:${gpu}:1" \
  --time="${time_limit}" \
  --output="${probe_dir}/slurm-%j.out" \
  --error="${probe_dir}/slurm-%j.err" \
  --export="ALL,PROBE_DIR=${probe_dir},CUDA_IMAGE=${image}" \
  "${remote_script}")"

printf 'JOB_ID=%s\nPROBE_DIR=%s\nGPU_GRES=%s\nTIME_LIMIT_MINUTES=%s\n' \
  "${job_id}" "${probe_dir}" "${gpu}" "${minutes}"

if [[ "${wait_for_job}" != true ]]; then
  exit 0
fi

deadline=$((SECONDS + minutes * 60 + 180))
while ((SECONDS < deadline)); do
  state="$(ssh "${TIGER_SSH_OPTIONS[@]}" "$target" squeue -h -j "${job_id}" -o %T | tr -d '[:space:]')"
  [[ -n "${state}" ]] || break
  printf 'STATE=%s\n' "${state}"
  sleep 5
done

ssh "${TIGER_SSH_OPTIONS[@]}" "$target" sacct -j "${job_id}" -X -n -P \
  --format=JobID,State,ExitCode,Elapsed,NodeList,AllocTRES

if ssh "${TIGER_SSH_OPTIONS[@]}" "$target" "test -f '${probe_dir}/result.txt'"; then
  result="$(ssh "${TIGER_SSH_OPTIONS[@]}" "$target" "cat '${probe_dir}/result.txt'")"
  printf '%s\n' "${result}"
  [[ "${result}" == "RESULT=PASS" ]]
else
  ssh "${TIGER_SSH_OPTIONS[@]}" "$target" "tail -80 '${probe_dir}'/slurm-*.out '${probe_dir}'/slurm-*.err 2>/dev/null" || true
  echo "RESULT=FAIL_OR_INCOMPLETE" >&2
  exit 1
fi
