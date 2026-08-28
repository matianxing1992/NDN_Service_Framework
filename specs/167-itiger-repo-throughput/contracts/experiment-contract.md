# Experiment Contract

## Frozen population

- Nodes: exactly two distinct TigerCluster compute nodes.
- Resources: CPU-only; no GPU request.
- Sizes: 64 MiB and 1 GiB.
- Replica/concurrency: r1/c1.
- Subjects: physical-network, raw-segmented-ndn, legacy-exact-packet,
  digest-only, signed-manifest.
- Sampling: one warmup plus five measured repetitions per subject/size cell.
- Duration: each rate observation covers at least 60 seconds.

## Path isolation

Measured payload, repository store, database, and cold destination paths MUST
be rank-local and MUST NOT resolve beneath `/project` or `/home`.  Shared
project storage may contain only immutable source/input manifests, small
coordination records, logs, ledgers, and promoted evidence.

## Source closure

The checksum-bound source manifest MUST include every Python module imported
from the mounted experiment source. In particular,
`Experiments/spec164_artifact_campaign.py` is a runtime dependency of the
Spec 164 roles used by `NDNSF_DistributedRepo_Artifact_Minindn.py`; omitting
it is a packaging failure, not a repository-transfer result. The exact local
candidate gate validates this closure before staging or submission.

## Matching

Within a size/repetition pair, all subjects use the same nodes, interface,
route, payload digest, packet payload size, and frozen environment.  Cache
states and sizes are never pooled.

## Failure semantics

No measured failure is silently retried, deleted, or replaced.  A new attempt
requires a new submission and campaign identity and cannot replace the original
evidence.  Missing or duplicate terminal records fail acceptance.

## Claim boundary

This campaign measures DistributedRepo artifact transport.  It does not prove
Qwen inference latency, model loading, GPU scaling, or multi-replica behavior.
