# Allocation topology and process-supervisor contract

## Placement classes

### `single-node-multi-gpu`

- exactly one allocated compute node and one job-scoped NFD;
- one Controller process, one User process, and three distinct Provider
  processes/identities;
- Stage 0/1/2 mapped to distinct allocated GPU UUIDs;
- local provider containers bind the same node-local NFD socket/state path;
- dependencies still use the NDNSF/NDN data path, but no cross-node claim exists.

### `multi-node`

- at least two allocated compute nodes and exactly one NFD per unique node;
- the same logical Controller/User/three-Provider role graph;
- one or more stage dependencies cross an explicit NFD face/route;
- TCP is the default selected transport; UDP is diagnostic unless the candidate
  selects UDP;
- placement change creates a new candidate/cell identity.

## Frozen process map v1

Each process entry binds:

```text
processId, kind, role, identityRef, nodeRank, taskRank, gpuRank/gpuUuid,
nfdSocket, commandDigest, readinessInputs, readinessOutput, shutdownOrder
```

The launcher MUST use Slurm-managed steps (`srun` or a generated multi-program
map), not background login-node processes. One node supervisor owns its NFD and
children, uses bounded readiness barriers, captures PID/task/exit records, and
terminates the process group on normal exit, TERM, INT, timeout, or partial
startup failure.

## Readiness order

1. scratch, binds, SIF, GPU mapping;
2. one NFD per unique node;
3. selected-transport faces/routes where multi-node;
4. ServiceController and identity/certificate service;
5. Provider permissions, artifact/backend readiness, and capability publish;
6. User permission readiness;
7. candidate request.

No later phase starts on partial readiness. Failure records the last satisfied
barrier and tears down all started children.

## Evidence

Retain requested and actual placement, node/task/GPU maps, NFD configs/PIDs,
faces/routes, process commands by digest, readiness timestamps, per-process exit,
selected and diagnostic transport results, and zero-survivor audits.

## Post-Spec-111 process map v2

The v1 map above remains the immutable pilot schema for existing candidates and
evidence. A post-Spec-111 candidate MUST use a separately versioned v2 map; it
MUST NOT reinterpret a v1 three-Provider record as proof of the new deployment
workflow.

The v2 role graph and Provider cardinality are derived from the exact
`DeploymentRevision`, including minimum-ready counts and selected placement.
There is no fixed three-Provider assumption. Every project process—NFD,
ServiceController, APPDeployment coordinator, generic APPProvider agent and
APPClient—MUST execute through the canonical runner and the exact SIF checksum
bound by `RuntimeAllocationHandoff`; host-side project executables are
forbidden. Infrastructure-only helpers such as `srun`, Apptainer and bounded
supervision remain host responsibilities.

Each v2 entry additionally binds:

```text
handoffVersion, deploymentId, revisionId/revisionDigest, sifSha256,
statePartition, nodeRunPath, providerBootEpoch, roleCardinality,
slurmGpuBinding, visibleGpuUuidSet, lifecycleBarrier
```

The supervisor starts one NFD per node, selected faces/routes and the
ServiceController, then starts generic Provider agents. Only after those agents
publish boot-epoch capabilities may APPDeployment apply the revision, stage and
warm external artifacts, reach `READY`/`ACTIVE`, and admit APPClient requests.
Slurm `RUNNING` is infrastructure state only; it never implies deployment
`READY`/`ACTIVE` or request success.

The v2 bind set is explicit and least-privilege:

- exact release/SIF, model, artifact and role identity inputs are read-only;
- APPDeployment and APPClient receive identity-partitioned writable persistent
  state roots under `/project/$USER/ndnsf-di/state/...`;
- processes on one node share only a job-unique writable node-run directory for
  NFD sockets and bounded coordination;
- allocation scratch and evidence staging are job-unique and writable;
- a broad writable `/project` bind is forbidden.

On TERM, preemption or time limit, the supervisor requests bounded APP drain,
flushes journal/evidence cursors and tears down in reverse dependency order.
Forced termination remains an uncertain fault recovered through leases, boot
epochs and orphan cleanup; it never triggers an automatic Slurm resubmission.

Single-node v2 acceptance precedes multi-node use. Multi-node v2 is eligible
only after the exact selected NFD transport/network-security probe passes and
must retain one real cross-node dependency in its evidence.
