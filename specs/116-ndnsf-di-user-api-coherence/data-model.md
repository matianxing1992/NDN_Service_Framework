# Data Model: Coherent NDNSF-DI User API

## DeploymentDefinition

Immutable, Application-authored intent created through
`InferenceApplication.define(...)`. The public value is readable and
serializable, but direct construction is not the canonical authority path.

| Field | Meaning | Validation |
|---|---|---|
| `schema_version` | Version of the signed definition contract | Supported explicit version |
| `application_identity` | NDN identity authorized to author this intent | Matches signer and configured policy |
| `deployment_owner` | NDN identity authorized to execute lifecycle operations | Explicit Application authorization; not a definition author |
| `deployment_id` | Stable logical deployment identity | Non-empty canonical identifier |
| `service` | Unified NDN service name advertised to requesters | Valid canonical service name |
| `model_intent` | Exact model or bounded authorized alternatives | Immutable typed descriptors; no policy-selected value yet |
| `artifacts` | Digest-bound external model/tokenizer/config references | No embedded secrets or executable policy objects |
| `request_contract` | Typed input/output and continuous-output semantics | Versioned schema identifiers and limits |
| `objective` | Typed optimization objective/SLO | Known metrics, units, priorities, and aggregation |
| `constraints` | Hard resource, placement, latency, privacy, availability, and allowed-partition bounds | Explicit units and satisfiability checks |
| `optimization_profile` | Allowlisted local suite profile name/config reference | Resolved locally; not Python code |
| `metadata` | Non-controlling application annotations | Size-bounded and unable to override typed fields |
| `created_at` / `expires_at` | Definition validity interval | Aware canonical time and bounded lifetime |
| `previous_revision` | Optional authorized upgrade/rollback lineage | Same deployment and valid digest |
| `signer_key_id` / `signature` | Application authenticity | Signature covers canonical bytes and passes creator authorization |

Concrete model choice, partition graph, stage roles, Provider assignment,
execution target, and live resource state are deliberately absent. They are
outputs of the selected policy suite and are bound into revision/decision
evidence. A deployment owner may validate and execute a definition but cannot
construct, re-sign, or semantically modify it.

`definition_digest` is computed over the schema-versioned canonical intent
fields excluding `signature`. The Application signature covers that digest plus
`application_identity`, `deployment_owner`, and the validity interval. The
signed envelope and outer NDN Data signature are both verified, avoiding a
signature/digest cycle while preserving offline handoff and network provenance.

## DeploymentDefinitionRef

Serializable, non-administrative reference to the exact Application-signed
definition Data. It contains Application identity, authorized deployment-owner
and coordinator service identity, deployment/service identity, definition Data
name and digest, validity interval, schema version, and signer key ID. It
identifies Application-authorized intent that NDNSF-DI may realize;
it is not requester authorization. Existing controller permissions, NAC-ABE,
tokens, signatures, and replay checks still decide whether a requester may ask.
The reference grants no right to change the definition, choose Providers, issue
lifecycle receipts, or claim readiness.

Remote request sends this reference to the bound authorized coordinator.
The requester neither resolves the definition's optimization profile nor loads
Application policy code. If the coordinator is unavailable and no ACTIVE
revision exists, the ON_DEMAND request fails or times out without side effects.

## DeploymentRevision

Immutable policy-resolved deployment identity containing:

- the signed definition digest and Application creator identity;
- exact OptimizationSuite descriptor and state digest;
- validated model variant, partition graph/roles, deployment action, Provider
  bindings or bounded binding rules, execution target/RunnerAdapter identity,
  artifact/runtime digests, and policy decision evidence;
- schema/lifecycle epoch, creation time, and optional predecessor;
- revision digest covering all preceding facts.

Request-time scheduling, admission, cache, recovery, and tuning decisions may
remain request-scoped, but their policy/suite versions and allowed bounds are
fixed by the revision and later bound by the execution certificate.

## DeploymentActivationRecord

Versioned APP-level discovery record transported as existing signed NDN Data at
an Application/deployment/revision-scoped name. NDNSD may advertise only its
name and digest as an untrusted lookup hint.

| Field | Meaning |
|---|---|
| `schema_version` | Activation record codec version |
| `record_name` | Canonical signed Data name |
| `application_identity` | Definition creator identity |
| `deployment_owner` | Authorized lifecycle owner/signing identity |
| `deployment_id`, `revision`, `service` | Exact invocable revision identity |
| `definition_record_name`, `definition_digest` | Exact Application-signed definition Data binding |
| `revision_digest` | Resolved-revision binding |
| `activation_certificate_digest` | Complete deployment activation/Provider binding evidence |
| `state`, `lifecycle_epoch` | ACTIVE state and monotonic fence |
| `activated_at`, `expires_at` | Freshness interval |
| `supersedes` / `revocation` | Rollover and explicit invalidation linkage |
| `signer_key_id`, `signature` | Deployment-owner signature over canonical bytes |

Catalog validation also verifies the referenced Application-signed definition,
owner authorization, complete activation certificate, and exact Data digest.
The record is descriptive invocation evidence, not a new capability or a
second NDNSF request protocol.

## DeploymentHandleRef

Serializable identity for reopening a creator or requester bound view before or
after activation. It contains deployment ID, immutable revision/lifecycle epoch,
owner/requester identity, journal locator/digest, and schema version. It carries
no ACTIVE claim, activation certificate, secret, or administrative credential.

## DeploymentRef

Serializable immutable, invocable identity for an authenticated ACTIVE
deployment revision. It does not exist for PREPARING/FAILED/INACTIVE state.

| Field | Meaning |
|---|---|
| `deployment_id` | Logical deployment identity |
| `revision` | Immutable active revision |
| `service` | Unified invocable service name |
| `definition_digest` | Digest of accepted definition |
| `activation_certificate_digest` | Digest binding deployment activation to revision/providers |
| `activation_record_name` / `digest` | Authenticated record used for revalidation |
| `lifecycle_epoch` | Monotonic rollover/fencing epoch |

A reference carries no administrative credential. Rebinding validates local
configuration, announcement evidence, and current journal state.

## DeploymentSummary

Typed discovery result containing exactly one requestable deployment: an ON_DEMAND
`DeploymentDefinitionRef` or an ACTIVE `DeploymentRef`. It also includes
lifecycle/availability state, freshness, capabilities, model/artifact summary,
and signed evidence identity. It never contains raw wire dictionaries or
private keys. ON_DEMAND means “authorized intent may be prepared by a request,”
not “Provider is ready.”

## DeploymentHandle

Process-local bound view containing an always-available `DeploymentHandleRef`,
optional ACTIVE-only `DeploymentRef`, and a reference to the authorized
lifecycle/request owner. It exposes `status`, `refresh`, `wait_until_active`,
and handle-reference serialization. Accessing `.ref` before authenticated
ACTIVE evidence raises `DeploymentNotActive`; no empty/placeholder certificate
is created. Administrative rollback, drain, or delete are available only when
bound through an authorized application/deployment manager.

## RequestableDeployment

A normalized requestable deployment constructed from an Application-owned
`DeploymentDefinition`, signed `DeploymentDefinitionRef`, `DeploymentHandle`,
or ACTIVE `DeploymentRef`. For an ACTIVE deployment, the exact revision and
activation evidence are frozen before request publication. For an ON_DEMAND
deployment, the exact signed definition is frozen before publication and one
immutable policy-resolved plan revision is frozen during PLANNING. In either
case, the request-specific `ExecutionCommitCertificate` commits selected
membership, responsibility, leases, and fences before Provider preparation. It
does not claim model readiness. Exact `ProviderReadiness` for every required
role and a readiness barrier separately gate model-runner invocation.

The same value contract is accepted by `InferenceApplication.request` and
`InferenceClient.request`. Possessing a signed `DeploymentDefinition` does not
grant authorship: every caller revalidates its signature and authorization, and
only the configured Application role can create or re-sign one.

## ProviderDeploymentOffers and ProviderDeploymentOffer

`ProviderDeploymentOffers` is the typed NDNSF-DI envelope carried in one
existing normal ACK. It binds the request, attempt, Provider, observed/expiry
times, and a bounded tuple of `ProviderDeploymentOffer`, allowing one Provider
to advertise different availability for multiple candidate roles. It does not
replace the ACK or constitute a readiness certificate.

| Field | Meaning |
|---|---|
| `request_id`, `attempt`, `provider`, `role` | Exact candidate responsibility |
| `availability` | `READY`, `NEEDS_PREPARATION`, or `UNAVAILABLE` |
| `definition_digest`, `revision_digest` | Target known to the Provider, when any |
| `artifact_digests`, `adapter_identity`, `boot_epoch` | Exact READY claim binding |
| `capability_digest`, `capacity`, `lease_offer` | Bounded capability/admission evidence |
| `operation_status` | Optional current generic `ServiceOperationStatus` |
| `observed_at`, `expires_at`, `signature` | Freshness and Provider authenticity |

Each offer describes one role. `READY` is only a selection hint until the
coordinator revalidates a signed
`ProviderReadiness` snapshot after Selection. `NEEDS_PREPARATION` means the
Provider is willing/capable but has not fulfilled its role. `UNAVAILABLE` is
not selectable. READY-first ranking belongs to provider-selection policy.

## CollaborationOperationStatusSnapshot (NDNSF Core)

The existing `SelectionExecutionStatus` reply is extended additively with a
bounded tuple of latest `ServiceOperationStatus` entries. Each entry is keyed by
`operation_id` and binds the outer request, selection digest, selected Provider,
role/member, attempt and status epoch. It adds a monotonic sequence, generic
state (`QUEUED`, `RUNNING`, `WAITING_INPUT`, `DONE`, `FAILED`, `CANCELED`, or
`EXPIRED`), `progress_known` plus progress fraction, reason/message, freshness,
and optional size-bounded application-detail schema/payload. The explicit flag
distinguishes unknown progress from 0.0. Old readers ignore additive indexed
member fields; old Providers still return the base selection lifecycle.

The snapshot is signed by the selected Provider and validated before use.
Sequence/epoch comparisons reject duplicate, reordered, replayed, cross-role,
cross-attempt, and stale updates. Application details are observation only,
must not carry secrets, and cannot grant execution or readiness authority.
Provider reporting is available both before handler dispatch and through
`CollaborationContext`; requester query/watch/wait all read the same latest
snapshot rather than creating separate state stores.

## DeploymentProgress and DeploymentStatus (NDNSF-DI APP)

`DeploymentProgress` is a signed, monotonically sequenced per-role observation:

```text
ACCEPTED -> FETCHING -> VERIFYING -> LOADING -> WARMING -> READY
         -> FAILED | CANCELLED | EXPIRED
```

It is the NDNSF-DI projection of a generic collaboration operation status. It
binds request, attempt, definition/revision, role, Provider, lease/fence,
operation ID, stage sequence, progress fraction, reason, timestamps, expiry,
and optional artifact/status evidence. Duplicate and old sequence numbers are
idempotently ignored. Optional collaboration events may notify observers, but
the signed selection-status snapshot is the recovery source and never commit
authority.

`DeploymentStatus` is the authenticated authoritative snapshot returned by the
request handle/status query. It contains the aggregate PREPARING/READY/terminal
state, every required role's latest progress, missing/failed roles, revision,
readiness-certificate digest when complete, and monotonic coordinator epoch.
The underlying NDNSF selection-status query works for one or many selected
roles and allows recovery after event loss or requester restart.

## RequestTiming

Exactly one of:

- `timeout: timedelta` relative to local request creation; or
- `deadline: datetime` timezone-aware absolute instant.

Supplying both, neither where required, naive datetime, non-positive timeout,
or expired deadline is rejected without network mutation.

## RequestRef

Serializable request identity containing request ID, requester identity,
deployment/revision, creation epoch, and journal locator/digest. It carries no
new token or control authority.

## InferenceRequestHandle

Process-local bound view of a `RequestRef` and canonical request owner. State
transitions remain journal-backed:

```text
CREATED -> PLANNING -> PREPARING -> CERTIFIED -> EXECUTING
        -> COMPLETED | FAILED | CANCELLED | EXPIRED
```

Terminal states are immutable. `events()` reports lifecycle transitions;
`output_stream()` is reserved for model output and is not implied by events.
`result(wait_timeout=...)` and `result_async(wait_timeout=...)` differ only in
waiting style and return the same typed value.

While state is `PREPARING`, `deployment_status()` exposes the authoritative
aggregate/per-role `DeploymentStatus`. `events()` may include progress events,
but callers do not need to consume every event to recover current state. The
request deadline covers both PREPARING and EXECUTING.

## InferenceResult

One typed result for synchronous and asynchronous consumption:

- output payload/reference and encoding;
- deployment/revision/request identities;
- provider/stage execution evidence;
- timing and resource summary;
- success status and output metadata. Failed, cancelled, and timed-out terminal
  states are represented by typed `InferenceError` objects raised by `result()`
  and remain inspectable through handle status/events.

## OptimizationSuiteDescriptor

Stable local descriptor containing suite name/version, ordered policy
descriptors, policy configuration digests, and default/custom markers. Its
digest is derived canonically; callers do not invent `state_digest`.

## Typed Policy Contracts

Each policy retains a dedicated request/result with common base evidence but
typed payload fields. Metadata may hold bounded annotations only and cannot be
the sole carrier of required inputs.

## Provider Interfaces

- `InferenceProvider`: registers and serves named model runners.
- `ProviderAdminPort`: separately constructed advanced coordinator interface for
  stage/activate/drain/delete and authenticated lifecycle receipt/report
  operations. It is not reachable from a serving facade without independently
  configured administrative credentials and authorization.
- `EnsureDeploymentCoordinator`: internal APP orchestration port shared by
  request-triggered preparation and explicit prewarming. It delegates to the
  existing policy, lease, provider-admin, readiness, commit/abort, journal, and
  recovery owners and owns no parallel protocol or durable authority.
- `RunnerAdapter`: independent execution mechanics selected after policy
  decisions and execution-certificate validation.
