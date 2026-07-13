#!/bin/sh
set -eu
sif=''; sif_sha=''; project=''; scratch=''; identity=''
while [ "$#" -gt 0 ]; do case "$1" in --sif) sif=$2;shift 2;;--sif-sha256) sif_sha=$2;shift 2;;--project) project=$2;shift 2;;--scratch) scratch=$2;shift 2;;--identity) identity=$2;shift 2;;--) shift;break;;*) exit 2;;esac;done
[ -n "${SLURM_JOB_ID:-}" ] || { echo APPTAINER_RUN_REQUIRES_SLURM >&2;exit 3; }
actual=sha256:$(sha256sum "$sif"|cut -d' ' -f1); [ "$actual" = "$sif_sha" ] || { echo SIF_DIGEST_MISMATCH >&2;exit 4; }
[ "$#" -gt 0 ] || { echo WORKLOAD_REQUIRED >&2;exit 2; }
exec apptainer exec --cleanenv --nv --bind "$project:/project:rw" --bind "$scratch:/scratch:rw" --bind "$identity:/identity:ro" "$sif" "$@"
