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

Because one node may host NFD, Controller, User, and multiple Provider
processes, each short or long-lived step MUST request an exact process-sized
allocation (`--exact --ntasks=1 --cpus-per-task=1`) and allow co-resident steps
(`--overlap`). `--exclusive` is forbidden for these steps: it reserves the
whole node's CPUs/GRES for one NFD or Provider and can leave later siblings
waiting forever. GPU ownership remains explicit through each Provider's
`--gpus-per-task=1 --gpu-bind=map_gpu:<gpuRank>` and the UUID check below.

The topology launcher MUST receive an explicit shared `--workdir` containing
the sealed application bundle/configuration and must verify that directory on
each execution node before `exec`. Generated process scripts `cd` there before
launching commands, so relative model/config paths cannot resolve against the
submitter's login directory.

The supervisor MUST perform the per-node `--workdir` visibility check before
starting any NFD. Generated process launchers MUST be materialized through a
target-node `srun` step into that node's job scratch and executed from the
scratch copy; an evidence or submit-host path that is not mounted on a compute
node is a pre-start failure.

`nodeRank` is bound to the scheduler's allocation order. Before any NFD starts,
the supervisor and the direct route-configuration entry point MUST compare the
process-map node names, in rank order, with `scontrol show hostnames
"$SLURM_JOB_NODELIST"`. A missing, empty, or different sequence MUST fail with
`SPEC110_ALLOCATION_NODE_ORDER_MISMATCH` (or its more specific nodelist
preflight error). This is required because `srun --relative=<nodeRank>` selects
that scheduler order; MiniNDN's deterministic node creation must not stand in
for the real allocation ordering.

Before starting any NFD, the supervisor MUST also verify on each target node
that every non-NFD `identityRef` exposes readable `.ndn/pib.db` and
`.ndn/ndnsec-key-file` files. An identity source that is visible on the submit
host but absent or unreadable on a compute node is a pre-start failure; the
supervisor must not leave NFDs running while waiting for a later business
process to discover that binding error.

The source identity directory and its `.ndn` tree MUST contain no symbolic
links. The supervisor checks this on the target node before NFD startup, and
the generated launcher repeats the check before copying the identity. This
prevents a scratch `HOME` from retaining a symlink back to shared project
storage.

The map MUST not contain duplicate `(address, tcpPort)` or `(address,
udpPort)` endpoints. Before starting NFD, the supervisor MUST perform a
target-node IPv4 bind probe for each declared TCP and UDP port and fail at the
pre-start boundary when a listener is already present. The probe is a bounded
race detector rather than a distributed port lease; the NFD bind remains the
final authority and dynamic cross-job port allocation is a separate contract.

Each NFD configuration MUST also be materialized under the current job's
scratch directory. If the frozen command contains `--config PATH` or
`--config=PATH`, the launcher rewrites that argument to its scratch-local
configuration copy; a fixed `/tmp` configuration path must never be shared
between jobs.

Before launching each NFD, the supervisor MUST remove the corresponding
scratch socket on that target node and retain the node-rank-to-step PID. NFD
readiness requires both that PID to remain alive and a newly-created socket;
an old socket from a reused scratch directory must never satisfy the barrier.

Pre-start visibility, materialization, or map-render failures MUST still emit
`teardown.json` with the original exit code and `survivors: 0`; the absence of
started children is an observed zero-survivor result, not an omitted artifact.

### Identity and runtime environment

`identityRef` is a read-only source directory. The launcher MUST NOT use it as a
writable `HOME`, and MUST NOT inherit `NDN_CLIENT_PIB` or `NDN_CLIENT_TPM` from
the login/Slurm environment. Before `exec`, every non-NFD process copies its
own `identityRef` into a process-specific, mode-0700 directory under the
job-owned scratch path, requires `.ndn/pib.db` and
`.ndn/ndnsec-key-file`, then exports matching `HOME`,
`NDN_CLIENT_PIB=pib-sqlite3:<home>/.ndn/pib.db`, and
`NDN_CLIENT_TPM=tpm-file:<home>/.ndn/ndnsec-key-file`. Each NFD receives its
own scratch `HOME` and cleared NDN keychain variables. Missing identity input
is a pre-exec failure; a shallow readiness marker cannot waive it. The
launcher records `SPEC110_PROCESS_HOME_READY` only after this setup. The
offline `NDNSF_SPEC110_TEST_MODE=1` fixture path may bypass a non-existent
identity source for fake binaries only and is not a deployment mode.

If a process command carries the same `identityRef` as an explicit argv token
(`--identity PATH` or `--identity=PATH`), the generated launcher rewrites that
token to the process-specific runtime `HOME` after the copy. This prevents a
real executable from reopening the shared read-only source and bypassing the
isolated PIB/TPM. The original command remains digest-bound in the frozen map;
the rewrite is a deterministic launcher binding.

The NFD socket path must also be below the current job's `--scratch` directory;
an otherwise valid `/tmp/ndnsf-di-*` path from another job is rejected before
any directory or socket is created. In a real Slurm allocation, the scratch
basename MUST be `ndnsf-di-<SLURM_JOB_ID>` or begin with
`ndnsf-di-<SLURM_JOB_ID>-`; the offline test mode may use a fixture basename.

Before starting any NFD, the supervisor MUST bind an ephemeral IPv4 socket to
each declared node address on that address's target node. An address that is
syntactically valid but is not assigned on that node is a pre-start failure;
the check does not replace the later NDN face/route connectivity gate.

Every Provider process also requires a single `CUDA_VISIBLE_DEVICES` selector
and verifies, before `exec`, that `nvidia-smi -i` for that selector returns
exactly the map's `gpuUuid`. A missing selector, missing `nvidia-smi`, failed
query, or UUID mismatch is a pre-exec failure; seeing the UUID elsewhere in the
node's full GPU list is insufficient. The test-mode fixture may bypass this
hardware check only for fake binaries.

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
