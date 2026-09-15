# B187-TIGER Evidence

**Task**: T004 TigerCluster same-candidate promotion
**Batch**: B187-TIGER
**Status**: WAITING_EXTERNAL_INPUT

T004 was not started. Spec187 has no `LOCAL_PASS`: the regular base SIF,
host-gate manifest and two real local MiniNDN runs are still unavailable.
Submitting Slurm or building/uploading a TigerCluster SIF before that gate
would violate the pair identity contract, so no scheduler, Apptainer or
cluster process was launched. This record deliberately contains no Tiger
protocol result.

The next execution must reuse the exact pair manifest and candidate digests
from a fresh `B187-LOCAL-YOLO` `LOCAL_PASS`, then invoke the existing
`Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-sif-app.sh`
entrypoint with Apptainer 1.5.3 and retain scheduler/facility failures as
`UNQUALIFIED` or `WAITING_EXTERNAL_INPUT`.
