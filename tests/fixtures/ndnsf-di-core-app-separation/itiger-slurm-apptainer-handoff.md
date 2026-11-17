# Contract: Spec 111 to iTiger Slurm/Apptainer Handoff

## Verdict and scope

After Spec 111 is implemented, NDNSF-DI is deployable on iTiger as a bounded
Slurm batch or interactive-allocation workload. The Docker image is an OCI
build/distribution source only. iTiger compute nodes run an immutable SIF with
`apptainer exec --nv`; they do not require or run a Docker daemon.

This contract does not claim that iTiger is a public, always-on Docker service.
An allocation is finite and scheduler-owned. Remote use means submit/monitor/
collect through Slurm, or use an explicitly authorized interactive allocation;
it does not mean exposing a persistent NFD or application port by node IP.

Spec 111 defines and statically validates this handoff only. It MUST NOT build
or publish OCI, materialize/run SIF, invoke Docker/Podman/Buildah/Apptainer, or
contact/submit to iTiger. All Spec 111 distributed acceptance runs use MiniNDN.
The first real container build/runtime and iTiger experiment belong together to
the new Spec 110 candidate after Spec 111 implementation and MiniNDN gates pass.

## Ownership boundary

| Concern | Owner | Must not become |
| --- | --- | --- |
| OCI build, publication, SIF materialization and digest verification | Spec 110 release pipeline | APP/Core behavior or a mutable tag-only release |
| Slurm allocation, task/GPU placement and process launch | Existing operations-owned `slurm-apptainer` runtime adapter | `APPDeployment` policy, Core scheduler or login-node daemon |
| Definition/revision validation and model lifecycle inside live Provider agents | Spec 111 `APPDeployment` | Slurm/container/process supervisor |
| Request planning, certification, execution and durable handle | Spec 111 APPClient/Core/APPProvider | Slurm job status inference |
| NFD faces/routes inside one allocation | Spec 110 allocation topology adapter | public Internet service or persistent cluster daemon |

The runtime adapter launches infrastructure and generic agents. APPDeployment
selects already-live agents, applies one exact revision and waits for signed
READY/ACTIVE evidence. These are consecutive state machines, not competing
deployment implementations.

## Immutable handoff identity

Every iTiger launch binds one `RuntimeAllocationHandoff` containing:

- Spec 111 candidate/source revision and completed offline-gate digest;
- `DeploymentRevision` ID and canonical digest;
- exact OCI reference by digest, SIF SHA-256 and release manifest digest;
- model/tokenizer/artifact references and digests outside the image;
- Slurm cluster snapshot, partition/account/QOS/GRES and selected placement;
- allocation process-map schema/digest and selected transport-probe digest;
- identity-set digest, persistent state-root identity and evidence destination;
- render/submission identity and explicit live-submission authorization state.

Any change creates a new handoff/candidate identity. A pre-Spec-111 OCI/SIF may
prove the substrate but cannot prove the post-separation APP workflow. Historical
Spec 109/110 jobs and a local MiniNDN result are never relabeled as this candidate.

`InfrastructureAllocationHandle` records adapter, Slurm job/allocation ID,
handoff digest and scheduler state. It is not a `DeploymentOperationHandle`.
Slurm `PENDING` means no APP state exists; `RUNNING` means only that resources
were allocated; neither state implies revision READY/ACTIVE or request success.

## Storage and bind layout

The canonical iTiger layout is:

```text
/project/$USER/ndnsf-di/
  releases/<release-id>/runtime.sif          read-only during execution
  models/<model-id>/<revision>/              read-only
  artifacts/<candidate-id>/                  read-only deployment inputs
  identities/<candidate-id>/<role>/          role-specific read-only bind
  state/<candidate-id>/<deployment-id>/      persistent identity-partitioned rw
  evidence/<candidate-id>/<run-id>/          atomic final promotion target
  logs/<candidate-id>/<run-id>/              scheduler logs

$SLURM_TMPDIR or validated job-local scratch
  run/<node-rank>/                            shared node-local NFD socket/state
  staging/                                    model/cache/temp runtime data
  evidence/                                   job-local evidence staging
```

The container receives only explicit binds:

- SIF/release, models, artifacts and the process's exact identity: read-only;
- identity-partitioned Spec 111 state root: read-write at `/state`;
- job-local scratch: read-write at `/scratch`;
- one node-local run directory: read-write at the same in-container path for the
  node NFD and every local controller/provider/client process.

The whole `/project` tree and another role's identity/state partition are never
bound. Workloads write evidence to scratch; the host finalizer checksum-verifies
and atomically promotes it. RuntimeJournal writes are the only intended durable
in-job metadata writes. Model weights and large caches never enter the journal,
OCI or SIF.

## Allocation process map v2

The post-Spec-111 process map is derived from the immutable deployment revision;
the generic schema MUST NOT hard-code three Providers or one Qwen partitioning
layout. A particular Spec 110 Qwen cell may freeze three stage Providers, but
that cardinality is candidate data rather than platform logic.

Each process entry binds:

- exact SIF digest and canonical container-runner digest;
- kind, role, NDN identity, node/task rank and shutdown order;
- assigned Slurm GPU UUID/device visibility, or explicit no-GPU status;
- node-local NFD socket bind and selected cross-node transport;
- revision/adapter/model/artifact digests and APP entrypoint arguments;
- readiness dependencies/output and evidence cursor.

Every NFD, controller, APPDeployment coordinator, APPProvider and APPClient
command runs inside the same verified SIF. The host supervisor may call `srun`,
Apptainer and routing/finalization helpers only; it MUST NOT resolve project
executables from the login/compute host environment. One Provider may see only
its assigned GPU set, and accepted mappings contain no duplicate GPU UUID unless
the revision explicitly declares safe sharing and tests it.

## Startup and use sequence

```text
render immutable handoff (no submit)
  -> explicit authorization and exactly-once sbatch
  -> allocation preflight, SIF/binds/GPU UUID verification
  -> one containerized NFD per allocated node
  -> selected-transport faces/routes for multi-node
  -> containerized ServiceController
  -> containerized generic APPProvider agents publish boot capabilities
  -> in-allocation APPDeployment validate/resolve digest check
  -> APPDeployment apply -> STAGING/WARMING -> signed READY -> ACTIVE
  -> APPClient submit/open/wait/result through one durable request handle
  -> APPDeployment drain -> INACTIVE
  -> process teardown, zero-survivor audit and atomic evidence promotion
```

No client workload starts on partial readiness. The apply inside the allocation
must match the already frozen revision digest; it is a verification/reconciliation
step, not a chance to resolve environment-dependent defaults differently.

## Network boundary

Single-node/multi-GPU is the first admissible iTiger path. All local processes
share one job/node-scoped NFD socket. Multi-node is enabled only after a bounded
allocation probe proves addresses, selected TCP or UDP reachability and exact
NFD face/route state. One NFD runs per node; local processes use its Unix socket,
and cross-node traffic uses allocation-scoped faces. Diagnostic failure of an
unselected transport is recorded but does not rewrite the selected transport.

No persistent NFD face, public listener, firewall change, login-node daemon or
post-allocation service is created. Teardown removes allocation routes/processes
and preserves only durable state/evidence allowed by retention policy.

## Scheduler failure and recovery

Slurm and APP states remain separately observable. On TERM/preemption/time-limit
warning, the supervisor requests APPDeployment drain with a deadline shorter
than the remaining allocation grace period, then flushes the journal and stages
evidence. If forced termination prevents graceful drain, Core leases, Provider
boot epochs and orphan cleanup preserve safety; the next allocation uses a new
infrastructure handle and reconciles only from the same authorized persistent
state/candidate identity.

A failed or preempted Slurm job is not automatically resubmitted. The existing
crash-safe submission journal and explicit-authorization rule remain controlling.
Scratch loss is expected; loss/corruption of the persistent state root blocks
new authority as defined by Spec 111.

## GPU and model boundary

Apptainer `--nv` injects the allocated host NVIDIA driver/devices. The SIF
provides compatible CUDA user-space, PyTorch and ONNX Runtime GPU libraries.
Acceptance requires an actual CUDA operation and observed
`CUDAExecutionProvider`; host `nvidia-smi` alone is insufficient and silent CPU
fallback is a failure.

Qwen weights, tokenizer and any ONNX/sharded export remain under `/project` and
are mounted read-only. Model size changes the Slurm resource/placement plan and
artifact reference, not the base image. Storage/quota and GPU-memory admission
remain per-size gates.

## Acceptance and controlling dependencies

Logical feasibility becomes executed evidence only after all of these pass:

1. Spec 111 implementation and its MiniNDN T144 workflow gate;
2. a new post-Spec-111 sealed OCI candidate and exact SIF materialization;
3. iTiger compute-node UID/Apptainer and RTX GPU runtime probes;
4. offline v2 handoff/bind/topology/lifecycle tests;
5. one exact single-node small-Qwen deployment using APP apply/submit/drain;
6. the selected-transport network probe before any multi-node candidate;
7. per-size model/storage/placement and correctness gates from Spec 110.

Only item 1 and static/offline handoff validation execute under Spec 111. Items
2-7, including the first container build, are Spec 110 work and require their
own candidate identity and authorization.

Passing proves bounded iTiger batch deployment/use, not production HA, public IP
service, physical UAV production, or indefinite availability.
