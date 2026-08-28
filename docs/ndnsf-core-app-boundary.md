# NDNSF Core/App Boundary

This document records which mechanisms belong in the reusable NDNSF core and
which mechanisms remain inside NDNSF-DistributedRepo, NDNSF-UAV-APP, and
NDNSF-DistributedInference.

## Boundary Principle

NDNSF core owns service-neutral network mechanisms:

- service invocation: request, ACK, selection, response, Targeted invocation,
  and trusted local invocation;
- security and bootstrap: controller-signed permissions, NAC-ABE routing,
  one-time tokens, certificate bootstrap, and negative ACK reason codes;
- reusable data movement surfaces: exact-name large-data retrieval and the
  continuous-publication stream substrate;
- reusable runtime state: provider capability hints, operation status,
  provider-owned admission leases, stream health, and network telemetry.

Applications own domain semantics:

- what a service means;
- how an object, mission, model, video frame, or tensor is interpreted;
- how to score domain-specific candidates;
- how to transform or execute application payloads.

The core should provide envelopes and validation helpers. Applications should
put domain-specific fields in typed payloads inside those envelopes.

## Core Mechanisms

### Provider Capability and Runtime Hints

Core owns the app-neutral answer to:

```text
Can this provider accept this service request now, and what is its current
runtime condition?
```

Reusable Python helpers:

- `ProviderCapabilityHint`
- `RuntimeHint` / `GenericProviderRuntimeHint`
- `GenericAckMetadata`
- `GenericAdmissionLease`
- `ProviderAdmissionLeaseTable`
- `RejectionReason`

Reusable C++ helpers:

- `ServiceProvider::ProviderCapabilityHint`
- `ServiceProvider::GenericProviderRuntimeHint`
- `ServiceProvider::GenericAckMetadata`
- `ServiceProvider::GenericAdmissionLease`
- `ServiceProvider::ProviderAdmissionLeaseTable`

Applications may attach service-specific payloads through
`service_payload_schema` and `service_payload`. For example, DI can attach model
fragment residency; Repo can attach storage capacity; UAV can attach camera
readiness. The core does not interpret those payloads.

Provider drain state is also core-level metadata. A provider may be reachable
and still advertise `DRAINING`, `PROVISIONING`, `MAINTENANCE`, or
`UNAVAILABLE`, which means new requests should avoid it while existing work may
still finish.

### Service Operation Status

Core owns the app-neutral lifecycle of long-running work:

```text
QUEUED -> RUNNING / WAITING_INPUT -> DONE / FAILED / CANCELED / EXPIRED
```

Reusable Python helper:

- `ServiceOperationStatus`

Reusable C++ helper:

- `ServiceProvider::ServiceOperationStatus`

Repo insert/fetch operations, UAV missions and recordings, and DI provisioning
or execution may all expose this shape. App-specific results belong in
`result_reference` or `metadata`.

When an operation produces a named object, use the core `DataProductReference`
shape to point at the exact NDN name, producer, service, object class, content
type, digest, size, and segment count. The application still defines what the
object means.

### Service Discovery Snapshot

Core owns a common view of provider availability:

- `ServiceDiscoveryRecord`
- `ServiceDiscoverySnapshot`
- `NdnsdHealthTracker`
- `NdnsdProviderState`

The discovery snapshot can be built from provider capability hints, NDNSD health
records, or raw controller/test dictionaries. It separates ready, draining,
stale, and unavailable providers. Applications decide how to score the ready
set; core only supplies consistent facts.

### Stream Health

Core owns the continuous-publication stream substrate:

- `StreamInfo`
- `StreamChunk`
- `StreamFecInfo`
- `StreamProducerBuffer`
- `StreamConsumerReorderBuffer`
- `StreamAdaptiveFetcherState`
- `StreamHealth`

Core stream helpers report session identity, stale sessions, gaps, duplicates,
backlog pressure, and adaptive fetch decisions. Applications still own codecs,
ROI logic, video bitrate policy, tensor semantics, and the actual FEC repair
algorithm.

Use streams for live or near-live continuous data. Use exact-name large-data
retrieval for complete named objects such as files, recordings, model
artifacts, manifests, and planned DI tensor bundles.

### Provider-Pair Telemetry

Core owns reusable network measurements between providers:

- `PeerNetworkMetric`
- `ProviderNetworkMatrix`

The matrix can estimate transfer cost and rank provider-pair candidates for
selection or cooperation. Applications decide how much those costs matter. DI
may use them for dependency exchange planning; Repo may use them for replica
placement; UAV may use them for multi-drone coordination.

## NDNSF-DistributedRepo Boundary

Repo owns:

- repo object manifests and catalog semantics;
- storage backends and persistence modes;
- replica placement policy;
- tombstones, TTL, catalog repair, and storage-specific status;
- STORE, FETCH, MANIFEST, INVENTORY, and catalog operations.

Repo should use core envelopes for:

- provider storage capability ACKs;
- operation status;
- admission or overload rejection;
- provider-pair telemetry for replica placement;
- exact-name large-data references for signed segmented Data.

Repo should not move its catalog or storage backend into core.

Repo selection policy still belongs to Repo. The core provides readiness facts
through `ProviderCapabilityHint` and `ServiceDiscoveryRecord`; Repo uses those
facts before applying its own storage-capacity and replica-placement policy. A
draining or unready typed provider hint must not be selected just because the
legacy storage fields report free capacity.

## NDNSF-UAV-APP Boundary

UAV owns:

- MAVLink and flight-controller semantics;
- mission planning, safety checks, and progress interpretation;
- camera readiness, H264/H265, ROI, YOLO, decoder queues, and bitrate policy;
- the actual FEC repair algorithm;
- ground-station GUI and operator workflows.

UAV should use core envelopes for:

- stream identity, chunk metadata, and stream health;
- command/mission operation status;
- readiness/capability ACKs;
- recording or data-product references through exact-name large-data paths.

UAV should not move MAVLink, camera, codec, ROI, or mission semantics into core.

## NDNSF-DistributedInference Boundary

DI owns:

- model split planning, stages, shards, roles, and merge logic;
- model fragment identity and residency semantics;
- exact forward cache, semantic cache, KV cache, and long-context policy;
- ONNX/LLM runtime backends and tensor bundle codecs;
- DI-specific scheduling and replanning policy.

DI should use core envelopes for:

- provider capability and runtime hints;
- admission leases and overload fast-fail;
- operation status for provisioning and execution;
- provider-pair telemetry for dependency exchange;

DI should not move model-specific planner logic, cache semantics, or tensor
formats into core.

Deployment lifecycle records should preserve DI-visible deployment fields such
as `deploymentId`, `planId`, `fragmentMap`, and legacy `status`, but they should
also carry core `ServiceOperationStatus` as `operationStatus`. Discovery and
sorting should prefer the core operation-status envelope when present, then fall
back to the legacy `status` field for older records.

## Accepted Boundary After Spec 084

Specs 085-090 completed the boundary migration. The accepted model is:

- Core owns service-neutral invocation, authorization, typed capability and
  operation envelopes, provider-owned leases, exact large-data transfer,
  continuous stream state, discovery facts, and provider-pair telemetry.
- DI, Repo, and UAV own all workload policy and domain payloads.
- The experimental generic coordination envelope was removed after the only DI
  advisory consumer failed its matched retention experiment. Multi-user DI now
  plans at each user and resolves contention through provider-owned leases,
  explicit rejection, and bounded replan.
- Typed `ProviderCapabilityHint` is authoritative. A bounded mixed-reader mode
  exists only until the next major release or 2026-12-31.
- Repo's native packet producer remains an internal Repo binding; it is not a
  public Core API.

The large `ServiceUser.cpp`, `ServiceProvider.cpp`, and UAV ground-station
container remain maintainability debt. They are not boundary violations and
must be split, if needed, under separate behavior-preserving refactor specs.

## Implemented Bridges

Completed bridge points:

- C++ core now has typed helpers for provider capability hints, service
  operation status, and data-product references, with round-trip tests on the
  existing generic ACK metadata path.
- Python core now has `ServiceDiscoveryRecord` and `ServiceDiscoverySnapshot`
  helpers that classify provider capability hints, NDNSD health records, and
  raw dictionaries into ready, draining, stale, and unavailable records.
- Repo ACK payloads keep the legacy storage fields and also carry
  `ProviderCapabilityHint` with storage capacity in `service_payload`.
- Repo `CAPABILITY` responses include `ProviderCapabilityHint`; main store and
  insert paths expose `ServiceOperationStatus` and, when a named object is
  produced, `DataProductReference`.
- DI provider ACKs carry `ProviderCapabilityHint` for ready, unavailable, and
  admission-rejected providers while keeping existing ACK fields.
- DI artifact provisioning ACKs carry `ServiceOperationStatus` for runtime
  install/materialization progress.
- DI `ProviderFragmentInventoryManager` can produce a core
  `ProviderCapabilityHint` from observed fragment residency.
- DI runtime-aware scoring can consume `ProviderCapabilityHint` directly as
  runtime metadata, so provider selection no longer requires DI-only
  `genericAckMetadata` when the core envelope is present.
- NativeTracer MiniNDN summaries decode typed DI provider ACK envelopes into
  `coreEnvelopeSummary`, including provider readiness, reason codes,
  service-payload schemas, operation states, and latest provider runtime view.
- DI `runtime_v1.py` now reuses the core telemetry classes for generic ACK
  metadata, runtime hints, admission leases, provider-pair metrics, and
  provider network matrices instead of maintaining duplicate local
  definitions.
- Repo Python clients prefer `ProviderCapabilityHint` over conflicting legacy
  ACK fields while retaining legacy-only fallback.
- Repo capacity selection now converts ACKs into core discovery records and
  skips typed unready or draining providers before applying Repo replica
  placement policy. Legacy-only ACKs still behave as ready fallback.
- Core C++ and Python stream helpers expose `StreamHealth`; UAV can map its
  video adaptive state to this helper while retaining H264/FEC/ROI policy.
- UAV shared protocol helpers now map flight command state, mission part
  state, mission progress, and recording data products into core
  `ServiceOperationStatus` and `DataProductReference` without moving MAVLink,
  camera, codec, FEC, ROI, or mission-planning semantics into core.
- Deployment ACK role capture now uses core ACK metadata parsing. A typed ready
  provider must pass `ServiceDiscoveryRecord.ready_for_new_request()` before its
  role is recorded; explicit `MODEL_UNAVAILABLE` negative ACKs remain valid for
  provisioning assignments during deployment.
- Deployment dictionaries now carry core `ServiceOperationStatus` in
  `operationStatus` while preserving the legacy `status` field. Deployment
  discovery sorting prefers the core status envelope and falls back to legacy
  status for older records.
- NativeTracer MiniNDN summaries now add core `ServiceOperationStatus` to
  `userExecution` and `dependencyExecution` while preserving legacy fields such
  as `status`, `reason`, request counts, latency metrics, and role lists.
- NativeTracer MiniNDN summaries now collect dependency-edge ndnping evidence
  into core `PeerNetworkMetric` and `ProviderNetworkMatrix` payloads under
  `providerPairTelemetry`, using producer-to-consumer dataflow direction for
  dependency exchange.
- NativeTracer runtime-aware planning can now consume a raw
  `ProviderNetworkMatrix` JSON file or a previous summary's
  `providerPairTelemetry.matrix` as dependency edge-cost input for a later run.
- UAV ground-station display now shows a concise `StreamHealth`-derived
  `stream_health` summary beside existing adaptive-video text, while preserving
  UAV-specific bitrate, decoder, FEC, ROI, and pressure details.
- UAV operational-layer state now includes `MissionPlanDocument`,
  `UavDataProductCatalogState`, `VehicleParameterSnapshot`, and
  `OperatorAuthorityLease`. These close the next QGroundControl-like workflow
  gaps without changing the core: persistent mission-plan metadata,
  repo/catalog-facing data-product summaries, compact vehicle parameter and
  capability views, and explicit operator command authority.
- The UAV parameter snapshot is an application view over MAVLink/PX4/ArduPilot
  details. A future NDNSF core configuration envelope may provide generic
  key/value status vocabulary, but MAVLink parameter ids, calibration policy,
  and flight-mode meaning remain UAV-owned.
- The UAV operator lease is currently an application authority model. If Repo,
  DI, and UAV converge on the same multi-operator conflict pattern, the
  reusable lease freshness/proof envelope can move to core while UAV keeps the
  meaning of `monitor`, `control`, `mission`, and `admin` scopes.
- QGroundControl-like setup, fly, plan, and analyze panels remain UAV-owned
  because their meaning is tied to MAVLink, flight modes, vehicle parameters,
  operator safety policy, and ground-station workflow. NDNSF core should not
  learn MAVLink parameter names, arming checks, mission item semantics, or
  inspector message ids.
- UAV now has first-class contracts for the next QGroundControl parity slice:
  `VehicleParameterEditRequest`, `VehicleParameterEditResult`,
  `PreflightCheckItem`, `MavlinkMessageSummary`, and `UavAnalyzeSnapshot`.
  These are application payloads that can be carried by normal NDNSF
  Request/Response or status services. Core mechanisms still provide the
  security, service invocation, stream health, operation status, and provider
  discovery envelopes around them.
- A future reusable NDNSF setting/configuration helper may standardize generic
  key/value edit lifecycle states such as requested, accepted, applied, and
  verified. The actual MAVLink value type, target system/component,
  parameter-name limit, and safety validation stay in NDNSF-UAV-APP.

Follow-up work:

- Remove the bounded mixed ACK reader at its compatibility deadline after
  deployment telemetry shows no legacy producers.
- Provider-pair telemetry should keep using the core `PeerNetworkMetric` and
  `ProviderNetworkMatrix` facts. NativeTracer now records observed dependency
  edge RTTs this way and can consume a prior matrix during planning;
  app-specific scoring and richer bandwidth/loss probes are still
  workload-specific follow-up work.
Follow-up changes should remain workload-scoped and retain matched regression
and MiniNDN gates.
# Spec 111 completed boundary

Decision order: deployment → model variant → partition → Provider assignment →
scheduling/admission → tuning/cache → execution target → validated atomic
intent. Recovery selects only a transition; Core preserves the original
deadline, attempt/output epochs, certificate membership and exactly-one visible
result. Cache policy may emit affinity but never final compute placement.

The ten policy ports are `ModelVariantPolicy`, `PartitionPlanner`,
`DeploymentPolicy`, `ProviderAssignmentPolicy`, `SchedulingPolicy`,
`AdmissionPolicy`, `ExecutionTuningPolicy`, `CachePolicy`, `RecoveryPolicy` and
`ExecutionTargetPolicy`. `RunnerAdapter` is separate. The aggregate
`ndnsf-distributed-inference` package is compatibility-only.
