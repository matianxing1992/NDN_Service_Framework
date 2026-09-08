#!/usr/bin/env bash
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=2
#SBATCH --mem=4G
#SBATCH --gres=gpu:rtx_6000:1
#SBATCH --time=00:05:00
set -euo pipefail
[[ $# == 1 && -d "$1" && -n ${SLURM_JOB_ID:-} ]]
root=$(cd -- "$1" && pwd)
cd "$root"
sha256sum --check input-sha256.txt
out="$root/job-$SLURM_JOB_ID"
mkdir -m 700 "$out"
scratch=$(mktemp -d "${SLURM_TMPDIR:-/tmp}/yolo-reference.XXXXXXXX")
finish() {
    status=$?
    trap - EXIT
    rm -rf -- "$scratch"
    printf 'exitCode=%s\nscratchRemoved=%s\n' "$status" "$([[ ! -e "$scratch" ]] && echo true || echo false)" > "$out/termination.txt"
    exit "$status"
}
trap finish EXIT
hostname > "$out/hostname.txt"
printf '%s\n' "${CUDA_VISIBLE_DEVICES:?Slurm GPU missing}" > "$out/cuda-visible-devices.txt"
[[ $CUDA_VISIBLE_DEVICES != *,* ]]
/usr/bin/apptainer version > "$out/apptainer-version.txt"
[[ $(cat "$out/apptainer-version.txt") =~ ^1\.5\.3(-[A-Za-z0-9.]+)?$ ]]
nvidia-smi --query-gpu=index,uuid,name,driver_version,memory.total --format=csv > "$out/gpu-inventory.csv"
df -B1 "$scratch" > "$out/scratch-capacity.txt"
available=$(df -B1 --output=avail "$scratch" | tail -1)
(( available >= 4600000000 ))
dd if=/dev/zero of="$scratch/write-probe" bs=1M count=1 conv=fsync status=none
rm "$scratch/write-probe"
cp --reflink=auto /project/tma1/ndnsf-di/candidates/spec180-runtime-b6710fd6/spec180-runtime.sif "$scratch/runtime.sif"
printf '%s  %s\n' b6710fd696a7f962f67f67a54278d92a15babb856c4af428a3f04ba54dc83285 "$scratch/runtime.sif" | sha256sum --check > "$out/sif-check.txt"
mkdir -m 700 "$scratch/home"
timeout --kill-after=10s 120s /usr/bin/apptainer exec --nv --cleanenv --containall \
  --home /node/home --pwd /probe \
  --bind "$scratch:/node" --bind "$root/bundle:/probe:ro" \
  --bind "$out:/output:rw" \
  "$scratch/runtime.sif" /usr/bin/env "CUDA_VISIBLE_DEVICES=$CUDA_VISIBLE_DEVICES" \
  PYTHONNOUSERSITE=1 /opt/venv/bin/python /probe/yolo_reference_probe.py \
  --backend cuda --package /probe/model --repository /probe/repo \
  --output /output/reference > "$out/application.log" 2>&1
