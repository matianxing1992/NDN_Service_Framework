#!/bin/sh
set -eu
[ -z "${SLURM_JOB_ID:-}" ] || { echo LOGIN_PREFLIGHT_INSIDE_ALLOCATION >&2; exit 3; }
partition=${1:-bigTiger}
command -v sinfo >/dev/null; command -v sbatch >/dev/null; command -v sacctmgr >/dev/null; command -v apptainer >/dev/null
[ -d "/project/$USER/ndnsf-di" ] || { echo PROJECT_ROOT_MISSING >&2; exit 3; }
sinfo -h -p "$partition" -o '%P|%N|%G|%T'
scontrol show partition "$partition"
sacctmgr -n -P show assoc "user=$USER" format=Account,QOS,MaxTRES,MaxJobs
apptainer version
