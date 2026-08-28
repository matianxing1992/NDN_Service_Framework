# Quickstart: Validate NDNSF-DI Core/APP Separation

This guide defines validation order; implementation commands are finalized by
the tasks and must not be used to authorize an iTiger job.

## 1. Resolve the feature and inspect source state

```bash
cd /home/tianxing/NDN/ndn-service-framework
cat .specify/feature.json
git status --short --branch
codegraph status .
```

Expected: Spec 111 is active, CodeGraph is current, and unrelated changes are
identified rather than modified.

## 2. Validate Spec Kit structure

```bash
.specify/scripts/bash/check-prerequisites.sh \
  --json --require-tasks --include-tasks
```

Expected: all required artifacts resolve, with unique FR/SC/task IDs, no placeholders, and complete
traceability.

## 3. Run static ownership and compatibility gates

```bash
python3 tests/python/test_ndnsf_di_architecture_imports.py -v
python3 tests/python/test_ndnsf_di_compatibility_manifest.py -v
python3 tests/python/test_ndnsf_di_decision_point_inventory.py -v
python3 tests/python/test_ndnsf_di_installation_profiles.py -v
```

Expected:

- zero forbidden DI Core imports;
- every legacy surface has one canonical owner/target;
- every production selection/optimization site maps to one public Python port,
  versioned default, validator and test, with zero native-only competing sites;
- Core-only import loads no APP/planner/model/GUI/experiment/ops implementation;
- no writer of `NDNSF_COLLAB_ROLE_PROVIDER_PREFERENCE` remains.

## 4. Run distributed execution and deployment consistency contracts

First preserve the current generic lease behavior, then run the new certificate,
crash and fencing gates:

```bash
python3 tests/python/test_ndnsf_execution_lease_table.py -v
python3 tests/python/test_ndnsf_di_execution_lease_transaction.py -v
python3 tests/python/test_ndnsf_di_execution_consistency.py -v
python3 tests/python/test_ndnsf_di_requester_recovery.py -v
python3 tests/python/test_ndnsf_di_deployment_fencing.py -v
```

Expected after the Spec 111 implementation tasks complete:

- prepare/commit never activates work without one complete authenticated
  `ExecutionCommitCertificate` over the exact Provider/role receipt set;
- dropped, duplicated and reordered prepare/commit/abort/release messages remain
  idempotent or fail closed;
- requester crashes before certificate construction produce no execution;
  same-identity restart after certification produces at most one visible result;
- higher attempt, Provider boot and deployment lifecycle epochs fence stale work;
- a network partition creates no new authority and leaves at most bounded
  in-doubt reservations that expire;
- competing deployment writers and destructive partial lifecycle actions fail
  closed;
- periodic cleanup reclaims expired leases, reservations, sessions, cache pins
  and runner handles even when the Provider receives no new request, while a
  bounded fencing tombstone still rejects delayed stale operations.

MiniNDN fault injection must cover every before/after boundary for prepare,
revalidation, commit, certificate publication, activation, result publication
and release. The acceptance property is atomic execution visibility and bounded
cleanup, not simultaneous global transition or availability during partition.

## 5. Run policy and assignment contracts

```bash
python3 tests/python/test_ndnsf_di_provider_assignment_policy.py -v
python3 tests/python/test_ndnsf_di_assignment_context.py -v
python3 tests/python/test_ndnsf_di_external_optimizer_sdk.py -v
python3 tests/python/test_ndnsf_di_model_adapters.py -v
python3 tests/python/test_ndnsf_di_engine_contract.py -v
python3 tests/python/test_ndnsf_di_model_variant_policy.py -v
python3 tests/python/test_ndnsf_di_scheduling_contract.py -v
python3 tests/python/test_ndnsf_di_tuning_cache_contract.py -v
python3 tests/python/test_ndnsf_di_execution_intent.py -v
python3 tests/python/test_ndnsf_di_optimization_observer.py -v
python3 tests/python/test_ndnsf_di_stream_recovery.py -v
python3 tests/python/test_ndnsf_di_least_input.py -v
```

Expected:

- fixed and cost policies make deterministic fixture decisions;
- non-default fixture implementations replace all ten Python policies and each
  changes its intended valid proposal/ordering;
- the process-local engine graph covers every policy epoch, invalidation and
  bounded recovery edge without creating a network coordinator;
- objective hard constraints dominate normalized metrics; missing units,
  aggregation or estimate horizons and stale/mixed snapshots are rejected;
- model-variant and execution-target proposal run only across authorized/
  compatible alternatives; assignment selects the final variant/plan/Provider/
  target tuple;
- ordinary one-role and multi-role assignments traverse the same
  `ProviderAssignmentPolicy` contract;
- both scheduling/admission scopes reject cross-scope mutation; provider-local
  batches follow adapter-declared mixed-phase or homogeneous compatibility;
- execution tuning accepts only declared typed/ranged parameters and cannot
  independently dispatch work;
- cache policy uses closed phase/action kinds and emits state actions/affinity
  without final compute assignment;
- deployment lifecycle tests cover idempotency, readiness, cooldown/residency,
  drain and active lease/session safety;
- every execution-intent fault boundary yields one complete certificate and one
  visible attempt, or no executable attempt with bounded resource cleanup;
- streaming recovery produces no duplicate/reordered visible output;
- observer delivery is idempotent, off-path and failure-isolated; later stateful
  decisions record policy state epoch/digest;
- policies/observer see workload shape but no prompt/tensor/secret/cross-tenant
  payload by default;
- recovery directives delegate reassignment/repartition/redeployment;
- partial suites resolve every omitted policy to an evidenced named default;
- frozen `DefaultOptimizationSuite` decisions match characterized current logic;
- Core rejects stale/infeasible/unauthorized/invalid-lease proposals;
- 100 overlapping paired requests have zero context bleed;
- no global preference environment mutation occurs.
- a standalone optimizer wheel uses only public SDK imports and is selected by
  an instance-scoped suite;
- unallowlisted, incompatible, timed-out, malformed and non-reproducible
  extensions fail closed with typed evidence;
- two suite instances complete 100 concurrent decisions with zero state bleed.
- Core imports no SDK code, and deployment/client/provider hook executors remain
  owned by APPDeployment/APPClient/APPProvider respectively.
- Runner adapters register in an instance `ExecutionAdapterRegistry`, are absent
  from `OptimizationSuite`, and are created only when selected by a validated
  `ExecutionTargetPolicy` result.

## 6. Validate clean external consumption

Build/install the separate Core, SDK, APP/planner and model-adapter wheels in
clean environments, then install the standalone fixture optimizer. Do not add
the repository root to `PYTHONPATH`. Build the native fixture from a separate
source/build directory against installed public headers.

Expected:

- fixture package requires zero NDNSF-DI source edits;
- direct registration and explicitly allowlisted entry-point loading work;
- Core and SDK inventories contain no forbidden implementations;
- owner wheel `RECORD` sets are disjoint; install/uninstall order does not remove
  files from another owner;
- only the compatibility wheel owns the root package `__init__.py` and legacy
  modules;
- native runner factory compiles/links without private headers or in-tree source;
- all ten policies and the independent Runner adapter plus optional outcome
  observer SPIs pass focused
  integration and the fixture's end-to-end choices
  reach one MiniNDN inference path.

Package allowlisting confirms the selected identity; it does not sandbox the
extension. Run only operator-trusted packages. Worker-process mode is validated
for failure/resource containment and late-result rejection.

## 7. Validate the complete deployment and invocation workflow

Use a persistent test state root and external artifact fixture; do not place
weights or secrets in the definition, revision, journal or image:

```bash
python3 tests/python/test_ndnsf_di_deployment_workflow.py -v
python3 tests/python/test_ndnsf_di_runtime_journal.py -v
python3 tests/python/test_ndnsf_di_request_handle.py -v
python3 tests/python/test_ndnsf_di_ops_cli.py -v
python3 tests/python/test_ndnsf_di_revision_upgrade.py -v
```

Then run the canonical MiniNDN workflow defined by the implementation task:

```text
validate -> resolve -> dry-run -> apply -> wait READY/ACTIVE
-> submit -> certified result -> restart/open request
-> drain -> wait INACTIVE
```

Expected:

- the same input resolves to one immutable revision digest; unresolved aliases,
  embedded secrets/weights and incompatible profiles fail before side effects;
- `deploy_plan()` delegates to metadata-only `prepare_session()` and never
  produces deployment readiness/activation evidence;
- dry-run mutates no Provider; duplicate/restarted apply converges by action ID;
- readiness binds required roles, revision/boot epoch, artifacts, adapter,
  permissions, capacity and fresh probes rather than liveness alone;
- generic Provider agents are launched by the test/operator harness before
  apply; apply selects them and performs revision-scoped staging/warming rather
  than pretending to provision OS/container/Slurm processes;
- corrupt/locked/unsupported/over-quota/non-persistent journal configurations
  block new authority and valid journal state reopens after APP restart;
- submit, sync/Future wrappers and reopened handles share request/attempt/
  certificate identity; the existing protected request wire envelope is durable
  before the handle returns, tamper/expiry fails closed, plaintext never enters
  journal/status, and cancellation is explicit and fenced;
- upgrade leaves existing attempts revision-bound, rollback creates a new epoch,
  and drain/delete preserves terminal evidence and shared external artifacts;
- CLI JSON and Python handles report identical revision/request/reason/evidence
  identities and static checks find no CLI-owned lifecycle logic.

### 7.1 Validate the iTiger handoff without submitting Slurm

Use the post-Spec-111 fixture and the existing Spec 110 Slurm/Apptainer adapter
contract to render, but not submit, one single-node handoff:

```text
DeploymentRevision + candidate/offline-gate digest
  -> RuntimeAllocationHandoff
  -> exact OCI/SIF/model/identity/state/process-map digests
  -> sbatch/process commands rendered only
```

Expected:

- Docker is referenced only as the OCI source; every iTiger project command
  executes through the exact SIF with Apptainer `--nv`;
- release/model/artifact/exact-role identity are read-only, `/state` is the
  identity-partitioned persistent RuntimeJournal root, and scratch/node-run are
  allocation-local read-write;
- no broad writable `/project` bind, cross-role identity/state bind, login-node
  process, direct Docker daemon call or mutable image tag is present;
- Provider count/roles derive from the revision, one NFD exists per node, local
  processes share the node-run socket bind and each Provider binds its declared
  Slurm GPU UUID set;
- scheduler, deployment and request status schemas remain separate;
- render creates no job. Live SIF/GPU/Qwen validation belongs to the linked Spec
  110 continuation and requires a new candidate plus explicit authorization.
- this step parses fixtures and renders/validates text only. It MUST NOT invoke
  Docker, Podman, Buildah, Apptainer, OCI/SIF construction, `sbatch`, `srun` or
  an iTiger SSH command.

### 7.2 Validation-environment boundary

Use local processes for unit, native, package-build and static handoff tests.
Use MiniNDN for every distributed network, security, fault-injection,
validate-to-delete and performance acceptance run. Do not use host/default NFD
as final evidence. Spec 111 produces no container image or remote job; the first
post-separation OCI/SIF build and iTiger execution are Spec 110 work after all
applicable MiniNDN gates pass.

## 8. Run behavior-preservation suites

Run the existing focused suites named in `tasks.md` for:

- runtime-aware planning and bounded replan;
- native plan/provider/dependency/cache/Qwen generation session;
- APP façade/deployment compatibility;
- NAC-ABE, permissions, token, replay and Targeted security;
- static container profile/manifest/template/handoff gates, without invoking a
  container runtime.

Expected: zero unexplained failures. A failure blocks migration/deletion and is
not explained away as refactor noise.

## 9. Run matched MiniNDN non-regression campaign

Use the frozen command, source identities, topology, 60-second measured window,
warmup, logging, timeout, sampler, ten-pair order and seeds recorded by the
implementation tasks. Execute each baseline/treatment cell exactly once; do not
automatically rerun a failed cell.

Expected:

- no correctness or completion-rate regression;
- median paired latency/throughput change and 95% paired bootstrap interval stay
  within the declared 5% non-regression margin;
- p50/p95, throughput, failures, resource/queue evidence and exact result path
  recorded;
- negative, neutral and failed cells retained.

## 10. Validate evidence lineage and rollback

```bash
python3 tests/python/test_ndnsf_di_candidate_lineage.py -v
python3 tests/python/test_ndnsf_di_compatibility_rollback.py -v
```

Expected:

- historical Spec 107/109/110 evidence digests unchanged;
- post-separation runs use a new candidate identity;
- compatibility aggregate can restore the last accepted behavior without data,
  authority or evidence migration.

## 11. Remote execution boundary

Do not submit Slurm work from this quickstart. A later candidate-bound iTiger
task must pass applicable offline source/runtime/container/SIF/model/network and
evidence gates, then receive explicit submission authorization.
Spec 111 also does not build OCI/SIF or run Docker/Podman/Buildah/Apptainer;
Spec 110 performs those operations together with the eventual iTiger campaign.
