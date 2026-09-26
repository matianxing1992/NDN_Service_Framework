#!/usr/bin/env bash
set -euo pipefail

host="${ITIGER_SSH_HOST:-itiger}"
[[ "${host}" =~ ^[A-Za-z0-9._-]+$ && "${host}" != -* ]] || {
  echo "Invalid SSH host alias" >&2
  exit 2
}

ssh -o BatchMode=yes -o ConnectTimeout=12 "${host}" '
set +e
root="/project/${USER}/ndnsf-di"
printf "SECTION=IDENTITY\nUSER=%s\nHOST=%s\n" "$USER" "$(hostname)"
printf "SECTION=PROJECT\nPROJECT_ROOT=%s\n" "$root"
df -hT "/project/${USER}" 2>&1
du -sh "$root" 2>&1
printf "SECTION=QUOTA\n"
quota -s 2>&1
if command -v lfs >/dev/null 2>&1; then
  lfs quota -h -u "$USER" /project 2>&1
else
  echo "LFS_QUOTA=UNAVAILABLE"
fi
printf "SECTION=LAYOUT\n"
for path in src images models cache manifests evidence; do
  target="$root/$path"
  if test -e "$target"; then
    stat -c "%A|%U|%G|%s|%n" "$target"
  else
    printf "MISSING|%s\n" "$target"
  fi
done
printf "SECTION=SLURM_GRES\n"
sinfo -N -o "%N|%P|%G|%f|%t" 2>&1
printf "SECTION=CONTAINER\n"
command -v apptainer 2>&1
apptainer --version 2>&1
printf "SECTION=TOOLS\n"
command -v python3 2>&1
command -v git 2>&1
command -v sha256sum 2>&1
'
