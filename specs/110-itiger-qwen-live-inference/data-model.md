# Data Model: NDNSF-DI iTiger Distributed Qwen Execution

## 1. RuntimeRelease

| Field | Rule |
|---|---|
| `releaseId` | digest-derived and immutable |
| `sourceRevision` | clean commit or sealed dirty snapshot |
| `ociDigest` | required; tag-only invalid |
| `sifSha256` | required after materialization |
| `dependencyLocks` | NDN, NDNSF, Python, CUDA, ORT, PyTorch |
| `compatibility` | driver/CUDA/ORT/PyTorch expectations |
| `secretScan` | zero disallowed findings |
| `probeStatus` | `NOT_RUN`, `PASS`, `FAIL` |

## 2. LiveClusterSnapshot

| Field | Rule |
|---|---|
| `observedAt` | timestamp for submission wave |
| `account`, `qos`, `partition` | discovered, not hard-coded authority |
| `gres` | exact live labels and node mapping |
| `apptainerVersions` | login and compute observations separated |
| `driverCuda` | actual compute observation |
| `storage` | actual path, capacity signal, quota command/result |
| `addresses` | allocation-scoped candidates only |

## 3. IdentitySet

Controller, User, and every Provider identity, certificate/name, policy digest,
expected prefix, project reference, bind target, owner/mode, and rotation ID.
Private key bytes are never represented in evidence.

## 4. AllocationTopology

| Field | Rule |
|---|---|
| `allocationId` | Slurm job ID and submission identity |
| `placementClass` | `single-node-multi-gpu` or `multi-node` |
| `nodes` | actual allocated nodes and addresses |
| `nfdInstances` | one per node; config/state/listener/PID |
| `facesRoutes` | explicit remote URI, prefix, face ID, route result |
| `processes` | controller/user/providers and `srun` task IDs |
| `gpuMappings` | GRES → physical UUID → container device → stage |
| `processMap` | Slurm task, node/GPU rank, PID, role, identity, command, readiness, shutdown order |
| `crossNodeEdges` | empty for single-node; at least one for multi-node acceptance |
| `teardown` | process, NFD, scratch, and login-node audit |

## 5. ModelArtifactSet

| Field | Rule |
|---|---|
| `family`, `size`, `revision` | Qwen2.5-Instruct locked identity |
| `tokenizerDigest` | required |
| `licenseDigest` | required |
| `dtype` | explicit |
| `sourceManifest` | filenames, bytes, hashes |
| `stages` | exactly the preregistered role artifacts |
| `interfaces` | tensor names, shapes, dtypes, dynamic axes |
| `oracleDigest` | prompt/output reference binding |

## 6. DistributedCandidate

Digest of RuntimeRelease, source, ModelArtifactSet, IdentitySet,
AllocationTopology template, service name, role graph, generation-session
contract, backend, resources, workload, and evidence schema. The candidate is
immutable after `FROZEN`.

States: `DRAFT -> VALIDATED -> FROZEN -> ELIGIBLE -> EXECUTED`.

## 7. ExperimentCell

Key: `(candidateId, mode, tokenLength, repetition, placementId)`.

Modes: `ORACLE`, `STAGED_BASELINE`, `DISTRIBUTED_CANDIDATE`, `DIAGNOSTIC`.

Lifecycle:

```text
PLANNED
  -> PREFLIGHT_BLOCKED
  -> READY_TO_SUBMIT
  -> SUBMITTED_NOT_STARTED
  -> CANDIDATE_EXECUTION_STARTED
  -> EXECUTED_PASS | EXECUTED_FAIL | EVIDENCE_INCOMPLETE
```

Only the two executed terminal states after the candidate execution-start proof
satisfy a live task. The placement-specific semantic validator decides whether
the recorded failure boundary is admissible. A replacement is a new cell/run linked by
`replacesRunId`; it never changes the original.

## 8. DistributedExecutionStartProof

Required fields:

- secured NDNSF request/session/attempt/lease identifiers;
- candidate and role-graph digest;
- at least one provider stage-start event with actual GPU UUID/backend;
- node/NFD/provider/stage mapping;
- timestamp monotonic ordering after allocation and readiness;
- evidence checksum and producer signature/reference.

For single-node PASS, all three distinct provider/GPU executions and all stage
dependencies must appear. For multi-node PASS, at least one dependency must
also cross a node boundary. A post-start FAIL records the last proven boundary;
a single stage start is never described as a complete distributed dataflow.

## 9. StageExecutionRecord

Role/stage, provider identity, node, NFD, GPU UUID, backend/provider, model
artifact digest, input/output tensor digests and bytes, queue/start/end times,
dependency fetch/publish names and timings, execution evidence, outcome/error.

## 10. GenerationSessionRecord

Request/user token, selection/provider token, lease/attempt, token epoch, KV
owner/state, deadline/cancellation, role states, dependencies, exact-final-once
counter, prompt/output token IDs and digests, terminal status/error.

## 11. PerformanceRecord

Raw per-request and per-stage samples plus cold load, warmup boundary, measured
window, TTFT, ITL, output tokens/s, request throughput, completion/failure,
dependency bytes/time, NDN/security/orchestration, CPU RSS, GPU memory/utilization,
scheduler wait, percentile sample counts, and unavailable-tail reasons.

## 12. EvidenceBundle

Checksummed directory containing source/candidate/release/cluster/allocation/
network/identity/security/model/stage/dependency/backend/GPU/output/timing/resource/
terminal/promotion records. `authority` contains independent substrate,
standaloneOracle, distributedCandidate, and physicalProduction dimensions.

## Invariants

1. A tag, filename, task checkbox, or `nvidia-smi` line is not candidate identity.
2. No candidate PASS without three distinct GPU/provider stage records; no
   multi-node PASS without a cross-node edge.
3. No task closure from a pre-start blocker.
4. No single-node overhead without a matched staged baseline; no multi-node
   placement overhead without the matched single-node NDNSF-DI cell.
5. No scratch deletion before atomic durable promotion verifies.
6. No physical-production PASS from Spec 110.

## 13. RuntimeAllocationHandoff

Immutable post-Spec-111 bridge containing candidate/offline-gate and deployment-
revision digests; OCI/SIF/release; model/artifact; Slurm profile/process-map/
selected-network; identity/persistent-state/evidence references; and render/
submission authorization identity. Any changed field creates a new handoff.

## 14. InfrastructureAllocationHandle

Operations-owned adapter, handoff and Slurm job/allocation identity with
requested/observed resources, scheduler state/reason and event/log cursor. It
may link to APP deployment/request handles only after those exist; scheduler
PENDING/RUNNING/COMPLETED never implies READY/ACTIVE/request success.
