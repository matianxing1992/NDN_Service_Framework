# Contract: Deployment and Invocation Workflow

## Purpose

This contract defines the public operator and application workflow after the
Core/APP separation. It connects declarative deployment input, Provider
readiness, immutable deployment revisions, request submission, distributed
execution certificates, result recovery and graceful teardown. It does not add
a cluster scheduler or a second execution protocol.

The contract reuses:

- the existing `DistributedInferenceDeployment` configuration/policy bundle;
- APP-owned `APPDeployment`, `APPProvider` and `APPClient` façades;
- `DistributedInferenceEngine` planning and validation;
- the canonical execution leases and consistency certificate;
- existing NDNSF security, V2/Targeted invocation and NDN Data transport.

Concrete Slurm/Apptainer materialization is operations-owned and is defined by
[the iTiger runtime handoff](itiger-slurm-apptainer-handoff.md). The APP API
consumes already-started generic Provider agents; it does not call `sbatch`, run
Docker or infer readiness from scheduler state.

## Canonical terms

The public workflow MUST distinguish these objects:

| Term | Meaning | Not equivalent to |
| --- | --- | --- |
| `DeploymentDefinition` | Mutable operator input loaded from the existing deployment configuration, including services, constraints and references | Running deployment state |
| `DeploymentRevision` | Immutable validated/resolved definition with one content digest and schema version | Provider instance or request attempt |
| `DeploymentInstanceRecord` | Mechanism-owned lifecycle state for one revision on an exact Provider set | Client plan/session metadata |
| `PreparedPlanSession` | Client-side reusable static plan/reference handle for one revision | Deployment activation |
| `InferenceRequestHandle` | Durable request/attempt/certificate/result rendezvous identity | Process-local `Future` |
| `RequestEnvelopeReference` | Name/digest/retention reference to an authenticated, confidentiality-protected request wire envelope | Raw prompt/payload in the journal |

`DistributedInferenceDeployment` remains the compatibility input façade for a
`DeploymentDefinition`. Existing `DeploymentSession` remains readable, but the
canonical APP SDK name is `PreparedPlanSession`. Existing `deploy_plan()` is a
compatibility alias for `prepare_session()` and MUST NOT claim that it loads,
activates or scales Provider processes.

## DeploymentDefinition and immutable revision

A definition contains or references:

- application, deployment and service identities;
- controller, trust schema, permission policy and requester/provider identity
  references;
- exact model identity or authorized model alternatives, tokenizer and request
  semantics;
- role graph, dependency/key scopes and plan constraints;
- optimization suite, policy configuration and Runner adapter identities;
- runtime/device/profile constraints and minimum ready Provider/role counts;
- external artifact references, cache/staging rules and artifact security;
- objective/SLO, admission and lifecycle bounds;
- persistent runtime-state root and evidence/event destinations;
- compatibility, rollout and rollback metadata.

Validation resolves this definition to one immutable `DeploymentRevision` with:

- schema/contract version and deployment ID;
- revision ID and canonical content digest;
- exact source/config/policy/adapter/runtime digests;
- exact model/tokenizer/artifact reference digests;
- selected compatibility matrix and required feature capabilities;
- creation identity/time and previous rollback revision, when present.

Mutable paths, environment-dependent defaults and secret values are not part of
the canonical digest directly. They resolve to explicit identities, mounted
locations or secret references before apply. Unknown fields, unresolved
references, incompatible profile versions and digest mismatches fail before any
side effect.

## External artifacts and secrets

Model weights remain outside packages and OCI/SIF images. Each
`ArtifactReference` declares:

- logical artifact/model/tokenizer identity and revision;
- URI or mounted path reference, digest, size, format and optional shard range;
- access-scope/credential reference, never the credential value;
- staging/cache destination class, quota and eviction ownership;
- executable flag and trust/signature evidence when applicable.

Providers stage artifacts from an operator-mounted volume, project store or
authenticated NDN reference. A Provider is not ready until required artifact
digests and adapter/runtime compatibility validate. Delete/unload releases
deployment-owned pins and temporary staging data but does not delete shared
external weights unless a separate explicit owner-authorized retention action
names them.

## Durable runtime journal

Requester crash recovery, lifecycle single-writer fencing and rollback require
durable APP-owned state. Spec 111 therefore defines one fixed mechanism-facing
`RuntimeJournal`; it is not an optimization policy, third-party SPI or cluster
database.

The default implementation is an append-only, versioned local filesystem
journal under an operator-configured persistent state root. Containers mount
that root as a persistent volume. A replacement APP process may recover only
when it has the same authorized identity and the same persistent root; Spec 111
does not promise active/active APPDeployment HA. Directories/files use
owner-only permissions. Each record is canonical, checksum-protected,
identity/digest bound, atomically replaced or appended with exclusive file
locking and durability appropriate to its transition. Records include:

- deployment revision and lifecycle/action certificates;
- requester coordinator binding and authenticated Provider receipts;
- execution commit certificate and result rendezvous reference;
- request envelope name/digest/expiry reference, never its plaintext content;
- rollback pointer, event sequence and cleanup/fencing tombstones.

The root is partitioned by canonical application/NDN owner identity and then by
deployment/request stream. APPDeployment operator records and APPClient
requester records need not share filesystem ownership; they exchange only
authenticated names/digests/receipts through existing protocol evidence. An
identity mismatch, symlink/path traversal or cross-tenant namespace access fails
before record or spool reads. A shared mounted volume is not a shared trust
domain.

The journal stores no private key, token, credential, raw prompt or tensor.
Security material remains in its existing owner. Corruption, lock loss,
unsupported version, quota exhaustion or non-persistent configuration fails
closed for new apply/commit authority and produces a typed operational reason.
Read-only status may continue from verifiable Provider/NDN evidence.

The journal retention window MUST dominate operation retry, certificate/result
rendezvous, rollback and fencing-tombstone windows. Compaction preserves the
latest revision/action state plus every still-authoritative request and cleanup
record. Operators may choose another implementation only behind the same
mechanism contract in a later feature; Spec 111 does not publish a swappable
state-store SPI.

Durable submission uses an adjacent owner-only request spool under the same
persistent root, or an operator-configured durable NDN repository. Before
`submit()` returns, the client serializes the existing RequestMessage wire
envelope, applies the existing authorization/confidentiality protection, writes
the protected bytes durably, and journals only a `RequestEnvelopeReference`
(Data name or local opaque locator, wire digest, security context and expiry).
If the envelope cannot be protected and retained through the request deadline,
retry and rendezvous windows, durable submission fails; it MUST NOT silently
return a reopenable handle backed only by process memory. Terminal retention
cleanup removes the protected spool object without exposing it to status,
metrics or optimizer inputs. This storage rule adds no new NDN wire name or
plaintext payload database.

## Public APPDeployment operations

The canonical APP SDK provides these semantic operations:

| Operation | Meaning | Side effects |
| --- | --- | --- |
| `validate(definition)` | Validate schemas, references, security/profile compatibility and state-root safety | None |
| `resolve(definition)` | Produce immutable revision and digest-bound plan | No Provider mutation |
| `plan(revision)` | Return intended lifecycle actions, target Providers and expected state changes | None; dry-run only |
| `apply(revision, idempotency_key)` | Prepare, certify and apply revision through the deployment consistency contract | Yes, fenced and journaled |
| `status(deployment_id, revision_id="")` | Return reconciled desired/observed state and reason-coded conditions | Read only |
| `wait(deployment_id, revision_id, condition, deadline)` | Wait for `READY`, `ACTIVE`, terminal failure or another explicit condition | Read only |
| `rollback(deployment_id, target_revision, idempotency_key)` | Apply a previously validated revision through a new lifecycle epoch | Yes; never rewrites history |
| `drain(deployment_id, deadline)` | Stop new admission and wait/cancel bounded active work before deactivation | Yes |
| `delete(deployment_id, revision_id, retain_artifacts=True)` | Remove inactive deployment-owned state/pins after safety checks | Yes; fail closed on active bindings |

Every mutating operation returns a durable `DeploymentOperationHandle` with
deployment/revision/action identities, lifecycle epoch, status and event cursor.
An idempotency key is scoped to operator identity, deployment, action kind and
target digest. Reusing it with a different revision/action digest is a typed
conflict, never a request to overwrite the old operation.
An APP restart reopens the handle from the journal and reconciles it with
authenticated Provider evidence; it never assumes success from a local return
value alone.

## Deployment lifecycle and readiness

Definition/revision state is separate from running instance state:

```text
DRAFT -> VALIDATED -> RESOLVED (immutable)

ABSENT
  -> APPLYING
  -> STAGING
  -> WARMING
  -> READY
  -> ACTIVE
  -> DRAINING
  -> INACTIVE
  -> DELETED
```

`Reconciling`, `Ready`, `Active`, `Degraded`, `Failed` and `Unknown` are
reason-coded conditions over the instance phase, not competing linear phases.
For example, an ACTIVE instance may be Degraded while still meeting minimums;
a partition makes unverifiable observations Unknown rather than implicitly
INACTIVE; a failed apply retains its last verified phase plus `Failed=True`.
Recovery repeats only the same idempotent action, starts an explicit new action/
lifecycle epoch, rolls back, or drains. `wait()` waits on a named condition and
returns the terminal failure/unknown reason rather than guessing from liveness.

`READY` and `ACTIVE` are different:

- `READY`: every required role has the declared minimum compatible Provider
  count, verified artifacts/runtime, permissions, current boot/revision epoch
  and successful readiness probe;
- `ACTIVE`: the ready revision is admitted for new client assignments.

`DEGRADED` means the revision is still serving within declared minimums but one
or more non-required replicas/dependencies are unhealthy. It never upgrades
unknown or stale facts to readiness.

Provider liveness alone is insufficient. A readiness condition binds service,
role, deployment/revision digest, Provider boot epoch, model/artifact/adapter
digests, capacity, permission state and observation freshness. Mixed revisions
are ineligible for one assignment unless the revision explicitly declares and
tests wire, model-state and dependency compatibility.

## Bootstrap and startup order

The supported startup sequence is:

1. Install compatible Core/APP/planner/ops and selected adapter profiles.
2. Mount external artifacts, persistent state root, trust material and
   operator-owned secret sources.
3. Run `validate` and `doctor`; verify NFD reachability, routes/faces, controller
   identity/trust schema, permissions, storage quota and adapter/device support.
4. Start or verify the existing NFD and `ServiceController` processes. APP does
   not become their process supervisor.
5. Through the external container/Slurm/systemd/operator layer, start generic
   APPProvider agents with an allowed owner profile and identity. They publish
   boot-epoch capabilities but are not READY for any revision. APPDeployment is
   not an OS/container/job scheduler and a missing Provider agent remains an
   explicit insufficient-capacity condition.
6. `APPDeployment.apply()` selects eligible live agents, certifies revision-
   scoped lifecycle actions, asks them to stage/verify artifacts and warm the
   selected adapter, then waits for signed revision-bound readiness before
   marking the revision active.
7. Start APPClient, resolve the active revision, construct its engine/suite and
   submit requests.

Failure at any step produces a reason-coded condition and a resumable operation
handle. Later steps cannot silently bypass an earlier failed gate.

## Public APPClient invocation operations

The canonical asynchronous path is:

```text
handle = client.submit(
    service=service_name,
    input=value_or_reference,
    deployment_revision=revision_id,
    objective=request_objective,
    deadline=deadline,
)

handle.status()
handle.wait(deadline)
handle.stream()       # only when the service contract supports it
handle.result()
handle.cancel(reason)

reopened = client.open_request(request_id, attempt_epoch=None)
```

`infer()` and `distributed_inference()` remain synchronous convenience methods
implemented through `submit().result()`. Process-local `Future` remains a
convenience adapter and is not the recovery identity.

`InferenceRequestHandle` binds requester identity, request ID, attempt epoch,
deployment revision, intent/certificate when available, deadline and result
rendezvous plus the protected request-envelope reference. `submit()` returns
the durable handle only after both journal and protected envelope are durable.
Its observable states are:

```text
CREATED -> PLANNING -> PREPARING -> CERTIFIED -> EXECUTING
        -> COMPLETED | FAILED | CANCELLED | EXPIRED
```

Status is monotonic for one attempt. `open_request()` reads the journal and
authenticated rendezvous evidence. It never reconstructs success from an
unverified cache or starts execution implicitly. Recovery may republish/replan
only from the digest-matching protected envelope and same requester identity;
missing, expired or unverifiable input evidence yields a terminal typed failure,
not an empty or reconstructed prompt.

Cancellation is attempt-fenced and idempotent. Before certification it aborts
or expires reservations. After certification it requests bounded cancellation
from Providers, prevents new dependent work and marks the attempt cancelled only
when Core visibility rules permit; already accepted terminal output is not
retroactively revoked. Timeout and client disconnect are not implicit
cancellation unless the request contract says so.

## Reconciliation, upgrade and rollback

`status()` reports both desired and observed state. APPDeployment periodically
reconciles non-terminal operations using journaled action identity plus current
Provider evidence; reconciliation may repeat only the same idempotent action or
advance through an explicit new lifecycle epoch.

An upgrade creates a new immutable revision. Providers advertise exactly which
revision they serve. Activation shifts new requests only after the new revision
is ready. Existing certified attempts remain bound to their original revision.
Rollback is an apply of a previous revision under a new lifecycle epoch; it does
not mutate the failed revision or reuse its certificates.

Spec 111 requires safe replace/rollback semantics but does not require traffic-
percentage canary routing, multi-cluster federation or an autonomous rollout
controller.

## Graceful shutdown

The supported shutdown sequence is:

1. mark the revision `DRAINING` and reject new engine/provider admission;
2. wait for active certified attempts until the bounded drain deadline;
3. cancel/expire remaining attempts through Core rules and preserve terminal
   evidence;
4. release leases, sessions, cache/model pins and temporary artifacts owned by
   the deployment;
5. flush journal/events and publish `INACTIVE` evidence;
6. stop APPProvider/APPClient processes; stop controller/NFD only under their
   external operator, after all dependent APP processes exit.

A forced process kill is a fault case handled by leases, boot epochs and orphan
cleanup, not the normal shutdown API.

## Operations CLI and observability

`ndnsf-di-ops` is a thin adapter over public APP APIs. It MUST provide semantic
commands equivalent to:

```text
ndnsf-di validate
ndnsf-di resolve
ndnsf-di plan
ndnsf-di apply
ndnsf-di status
ndnsf-di wait
ndnsf-di rollback
ndnsf-di drain
ndnsf-di delete
ndnsf-di doctor
ndnsf-di events
ndnsf-di metrics
ndnsf-di request submit
ndnsf-di request status
ndnsf-di request wait
ndnsf-di request result
ndnsf-di request cancel
ndnsf-di request stream
```

CLI code does not implement lifecycle, security or inference logic. Human and
JSON output share a versioned status schema. Every operation/request condition
includes observed generation, transition time, reason code, retryability,
responsible owner and evidence/journal cursor. At minimum diagnostics separate:

- invalid definition/revision/profile;
- NFD/controller/permission unavailable;
- artifact missing/digest/signature/quota failure;
- adapter/device/runtime incompatible;
- Provider insufficient, stale, warming or wrong revision;
- lifecycle conflict/fencing/partial apply;
- request admission/planning/lease/certificate/execution/result failure;
- journal corruption/lock/quota/non-persistent state root;
- drain timeout and orphan-cleanup pending/failure.

Metrics are bounded, tenant-safe and labeled by deployment/revision/service/
reason, not raw payload. Events are ordered per deployment/request stream and
deduplicated by event identity.

## Minimal Python flow

```python
deployment = APPDeployment.from_config(
    "deployment.yaml",
    state_root="/var/lib/ndnsf-di",
)
validation = deployment.validate()
validation.raise_for_errors()
revision = deployment.resolve()
operation = deployment.apply(revision, idempotency_key="release-42")
deployment.wait(operation.deployment_id, revision.id, "ACTIVE", deadline)

client = APPClient.from_config("deployment.yaml", state_root="/var/lib/ndnsf-di")
request = client.submit(
    service="/LLM/Qwen",
    input={"prompt": "..."},
    deployment_revision=revision.id,
    deadline=deadline,
)
result = request.result()

deployment.drain(revision.deployment_id, drain_deadline)
deployment.delete(revision.deployment_id, revision.id, retain_artifacts=True)
```

Concrete signatures may use typed constructors and context managers, but they
MUST preserve these identities, side-effect boundaries and recovery semantics.

## Acceptance gate

Before Spec 111 is considered deployable/usable, tests MUST prove:

- clean install to definition validation without repository path injection;
- immutable revision/digest and no secret/raw-weight embedding;
- dry-run produces no Provider mutation;
- apply is idempotent and survives APPDeployment restart at every phase;
- readiness rejects missing roles, stale/wrong revisions and bad artifacts;
- client submit/synchronous convenience/reopen/cancel/stream contracts share one
  request identity and certificate path;
- journal corruption, lock contention, quota exhaustion and ephemeral-state
  misconfiguration fail closed;
- upgrade keeps old attempts revision-bound and rollback creates a new epoch;
- drain blocks new work, bounds old work and cleanup preserves shared artifacts;
- CLI and Python produce the same versioned status/reason/evidence identities;
- one MiniNDN workflow completes validate -> apply -> ready -> submit -> result
  -> requester restart/reopen -> drain -> inactive with exact evidence.

This gate is separate from performance claims. Passing it proves an operable
workflow, not Qwen speed, production HA or iTiger deployment.
