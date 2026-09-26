#!/usr/bin/env bash
set -euo pipefail

host=${1:-itiger}
ssh -o BatchMode=yes "$host" 'bash -s' <<'REMOTE'
set -euo pipefail
printf 'observedAt=%s\n' "$(date -u +%FT%TZ)"
printf 'host=%s\n' "$(hostname)"
printf 'user=%s\n' "$(id -un)"
printf 'slurmVersion=%s\n' "$(scontrol --version)"
printf 'apptainerVersion=%s\n' "$(apptainer version)"
printf 'podmanVersion=%s\n' "$(podman version --format '{{.Client.Version}}')"
printf 'buildahVersion=%s\n' "$(buildah version | awk 'NR==1 {print $2}')"
podman info --format 'podmanRootless={{.Host.Security.Rootless}}'
podman info --format 'podmanDriver={{.Store.GraphDriverName}}'
podman info --format 'podmanGraphRoot={{.Store.GraphRoot}}'
podman info --format 'podmanRunRoot={{.Store.RunRoot}}'
scontrol show config | awk -F= '
  /^[[:space:]]*TmpFS[[:space:]]*=/ {
    gsub(/[[:space:]]/, "", $2)
    print "slurmTmpFS=" $2
  }
'
scontrol show nodes | grep -o 'TmpDisk=[^ ]*' | sort -u | sed 's/^/slurm/'
grep "^$(id -un):" /etc/subuid /etc/subgid 2>/dev/null | sed 's/^/uidMap=/' || true
printf 'projectUsage='; du -sh "/project/$(id -un)" | awk '{print $1}'
printf 'projectSharedCapacity='; df -h "/project/$(id -un)" | awk 'NR==2 {print $2 "/" $4}'
printf 'warning=shared_project_df_is_not_user_quota\n'
printf 'warning=login_observation_requires_compute_job_probe\n'
REMOTE
