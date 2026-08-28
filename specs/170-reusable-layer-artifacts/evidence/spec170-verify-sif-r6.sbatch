#!/usr/bin/env bash
# Verify the exact r6 SIF after the build wrapper reported cleanup failures.
# This job is read-only with respect to the released SIF: it hashes the file,
# executes the static probe, and records Apptainer metadata.
#SBATCH --job-name=spec170-sif-verify-r6
#SBATCH --partition=bigTiger
#SBATCH --nodes=1
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=4
#SBATCH --mem=16G
#SBATCH --time=00:15:00
#SBATCH --output=/project/tma1/ndnsf-di/campaigns/spec110/spec170-sif-verify-r6/slurm-%j.out

set -euo pipefail
unset APPTAINERENV_HOME APPTAINERENV_PWD SINGULARITYENV_HOME SINGULARITYENV_PWD
: "${SPEC170_RUNTIME_SIF:?}"
: "${SPEC170_RUNTIME_SIF_SHA256:?}"
: "${SPEC170_EVIDENCE_DIR:?}"
test "$(sha256sum "$SPEC170_RUNTIME_SIF" | cut -d' ' -f1)" = "${SPEC170_RUNTIME_SIF_SHA256#sha256:}"
mkdir -p "$SPEC170_EVIDENCE_DIR" "/tmp/ndnsf-di-${SLURM_JOB_ID}/probe-home"
probe_home="/tmp/ndnsf-di-${SLURM_JOB_ID}/probe-home"
probe_target="/home/$(id -un)"
env -i \
  PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
  HOME="$probe_target" XDG_CONFIG_HOME="$probe_target/.config" \
  XDG_CACHE_HOME="$probe_target/.cache" \
  SLURM_JOB_ID="$SLURM_JOB_ID" \
  apptainer exec --cleanenv --containall --no-home --pwd / \
  --bind "$probe_home:$probe_target" \
  --env HOME="$probe_target" \
  --env XDG_CONFIG_HOME="$probe_target/.config" \
  --env XDG_CACHE_HOME="$probe_target/.cache" \
  "$SPEC170_RUNTIME_SIF" \
  /usr/local/bin/ndnsf-di-probe-runtime --mode static \
  | tee "$SPEC170_EVIDENCE_DIR/static-probe.json"
apptainer inspect "$SPEC170_RUNTIME_SIF" >"$SPEC170_EVIDENCE_DIR/apptainer-inspect.txt"
printf 'SPEC170_SIF_VERIFY_PASS jobId=%s sif=%s\n' "$SLURM_JOB_ID" "$SPEC170_RUNTIME_SIF_SHA256" \
  | tee "$SPEC170_EVIDENCE_DIR/result.txt"
