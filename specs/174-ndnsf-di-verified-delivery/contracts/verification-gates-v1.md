# Verification And Promotion Contract v1

## Governing Rule

Gates are strictly ordered and identity-bound:

```text
U: unit
I: in-process integration
M: real MiniNDN + CPU
C: exact local candidate SIF
T: TigerCluster qualification
```

Gate `N+1` reads the immutable PASS manifest from gate `N` and rejects a different source tree, allowed-dirty-input set, dependency/config/input/oracle identity, or candidate where applicable. A higher gate never repairs or compensates for a lower failure.

## Common Harness Requirements

- fixed inspectable ONNX fixture, input, seed, policy, split candidates, and oracle;
- independent fresh processes for repeated acceptance runs;
- process-tree monitoring and bounded teardown;
- per-phase no-progress deadline, per-case hard deadline, and overall gate deadline;
- deterministic fault schedule for negative packet/process cases;
- exact source/dependency/config/input/oracle hashes;
- bounded manifest, first failure, relevant log, and trace hash;
- no secrets or plaintext tensors;
- no unlimited background campaign.

Correctness assertions are exact. Environment-sensitive duration is diagnostic and cannot be a pass oracle.

## Gate U — Unit

**Inputs**: current source, frozen contracts/fixtures, configured dependency toolchain.

**Mandatory coverage**:

- ACK closure immutability/dedup/late ACK;
- pure deterministic pre-split-first proposal;
- graph/split legality and complete coverage;
- bijective Provider/role mapping and tensor-rank group legality;
- plan commit before Selection and exact Provider projection;
- deterministic tensor names and manifest/segment codec validation;
- duplicate/reorder/corruption/replay/stale identity handling;
- dependency/collective state transitions and deadlines;
- exact grant/capability checks, admission, release, zeroization;
- cancel/replan/restart/fencing and final-result acceptance.

**Pass**: relevant C++ and Python unit suites pass from one consistent build; every required rejection has zero accepted output and zero execution authority. No skipped required case.

## Gate I — In-Process Integration

**Topology**: four distinct Provider runtime instances/process identities, one User, one repository/controller context, four complete roles. In-process transport may make the test deterministic but must use production codecs, handlers, state machines, Provider projections, ORT execution, and dependency APIs.

**Positive cases**:

- one-role baseline;
- four-role pipeline;
- two-rank tensor group;
- four-Provider hybrid `[1,2,1]`;
- complete final response equals frozen oracle.

**Fault cases**: deterministic loss/retry, reorder, duplicate, conflicting duplicate, corruption, stale attempt/plan/epoch/rank, replay, cancellation, missing peer/rank, late old output, and no-progress timeout.

**Pass**: each acceptance case passes three independent process runs; no hang, duplicate role execution, hidden fallback, partial final response, or leaked authority. Every planned edge has matching publish/fetch identity evidence.

## Gate M — Real MiniNDN + CPU

**Environment**: real MiniNDN network namespaces, separate processes, per-node NFD, configured routes/faces, small CPU ONNX fixture. Host-only NFD, mocked forwarder, or in-process result is invalid.

**Required cases**:

- one-role baseline sanity;
- multi-Provider pipeline;
- two-Provider/two-rank tensor group;
- four-Provider hybrid;
- selected deterministic packet loss/reorder/duplicate/corruption and missing peer/cancellation cases.

**Observations**:

- node/process/NFD identity;
- exact REQUEST/ACK closure/plan/Selection phases;
- every cross-Provider manifest and segment Interest/Data name;
- Provider-local ORT execution and final response;
- complete oracle match and no hidden file/socket cross-Provider dependency.

**Pass**: all positive cases pass three independent clean runs with fixed inputs/config/seeds; all negatives fail boundedly with first missing/invalid event/name and no accepted partial result.

## Gate C — Exact Local Candidate

**Build boundary**: local Apptainer from sealed source/dependency inputs. Native/Python extensions are built inside the candidate or ABI-identical sealed builder. Host-built extensions are disqualifying.

**Preflight**:

- build record and all input hashes;
- Python version/SOABI and real `import ndnsf._ndnsf`;
- `ldd`/RPATH closure with no missing or unintended host libraries;
- exact Boost, ndn-cxx, NDN-SVS, NFD/MiniNDN, ORT versions;
- ORT execution-provider inventory;
- CLI and MiniNDN entry points/cwd/artifact resolution;
- runtime has no required PyTorch/Transformers dependency;
- secret scan and writable-path policy.

**Execution**: run the same CPU positive/negative acceptance workload inside the exact SIF.

**Pass**: build/preflight/CPU manifests pass and identify one immutable SIF SHA-256. The SIF becomes read-only promotion input.

## Gate T — TigerCluster

**Admission condition**: Gate C PASS manifest and exact SIF/source/config/input/oracle hashes.

**Stage T0 — Read-only preflight**:

- VPN/SSH/login identity, project storage/scratch capacity and permissions;
- Slurm partitions/QoS/account, allocation shape, node health;
- Apptainer version, driver/CUDA/ORT compatibility, GPU inventory;
- inter-node/provider routing requirements and expected ports/interfaces;
- SIF/model/input/config/evidence hashes and cwd/artifact paths.

No expensive job is submitted if T0 fails.

**Stage T1 — CPU/no-GPU negative**: GPU-required config must fail closed with no CPU fallback; CPU reference route may run only where explicitly configured.

**Stage T2 — Single GPU reference**: one Provider/role completes the whole application result, exact oracle match, intended ORT GPU provider, zero CPU fallback.

**Stage T3 — Cross-Provider multi-GPU tensor**: at least two Providers own distinct rank roles; exact group membership; every cross-Provider tensor uses sealed NDN names; complete answer and oracle; intended GPUs; zero CPU fallback.

T2 and T3 each require three independent fresh qualification runs with identical frozen candidate, input, configuration, and oracle identities. An environment-readiness diagnostic does not count as one of the three.

**Optional T4**: hybrid or larger model only after T3 passes. It cannot replace T3.

**Pass**: complete T0–T3 evidence for the unchanged candidate. “model loaded,” “stage 0 complete,” ORT smoke, or GPU kernel activity alone is not PASS.

## First-Failure Taxonomy

Every failure records one primary code and the last successful phase:

- `SOURCE_OR_INPUT_MISMATCH`
- `BUILD_OR_LINK_FAILURE`
- `ACK_CLOSURE_FAILURE`
- `PLACEMENT_OR_PLAN_REJECTED`
- `SELECTION_OR_AUTH_REJECTED`
- `ARTIFACT_FETCH_OR_ASSEMBLY_FAILURE`
- `RESOURCE_ADMISSION_FAILURE`
- `DEPENDENCY_NAME_OR_MANIFEST_FAILURE`
- `DEPENDENCY_TIMEOUT_OR_MISSING_PEER`
- `COLLECTIVE_MEMBERSHIP_OR_ROUND_FAILURE`
- `ORT_EXECUTION_FAILURE`
- `FINAL_RESULT_OR_ORACLE_FAILURE`
- `SIF_ABI_OR_RUNTIME_CLOSURE_FAILURE`
- `TIGER_ENVIRONMENT_OR_ALLOCATION_BLOCK`
- `UNPLANNED_FALLBACK_OR_TRANSPORT`
- `HARNESS_TIMEOUT_OR_TEARDOWN_FAILURE`

## Promotion Manifest Decision

`PASS` requires every mandatory case and identity check. `FAIL` means product/harness behavior violated the contract. `BLOCK` is permitted only for an unavailable external environment/hardware/authority after local gates pass; it is not permission to skip or weaken a test. A repair produces a new immutable manifest and, after candidate construction, a new candidate digest.
