#!/usr/bin/env bash
set -euo pipefail

host="${ITIGER_SSH_HOST:-itiger}"

ssh -o BatchMode=yes -o ConnectTimeout=12 "${host}" '
set +e
printf "SECTION=IDENTITY\n"
hostname
whoami
id
printf "SECTION=STORAGE\n"
df -hT /home /project /tmp
findmnt -T /tmp -o TARGET,SOURCE,FSTYPE,OPTIONS
findmnt -T /project -o TARGET,SOURCE,FSTYPE,OPTIONS
printf "SECTION=SLURM_NODES\n"
sinfo -N -o "%N|%P|%G|%f|%t"
printf "SECTION=SLURM_PARTITION\n"
scontrol show partition bigTiger
printf "SECTION=SLURM_ASSOCIATION\n"
sacctmgr -n -P show assoc where user="$USER" \
  format=Cluster,Account,User,Partition,QOS,GrpTRES,MaxTRES,MaxJobs,MaxSubmit
printf "SECTION=CONTAINER\n"
command -v singularity
singularity --version
command -v apptainer
apptainer --version
'
