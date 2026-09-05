# NDNSF Architecture

Before changing a cross-layer runtime, DI, deployment, or validation path,
follow [`architecture-reading-guide.md`](architecture-reading-guide.md) and
inspect the newest entry in [`failure-log.md`](failure-log.md). This document
is the concise ownership map; the active Spec, contracts, source, and evidence
remain authoritative for a particular feature.

NDNSF is organized as a framework core plus application layers that validate
and stress the core mechanisms.

```text
Applications
  UAV workload, distributed inference, repo clients, examples

NDNSF application packages
  NDNSF-DistributedInference
  NDNSF-DistributedRepo
  pythonWrapper

NDNSF core runtime
  ServiceController
  ServiceProvider
  ServiceUser
  ServiceContainer
  LocalServiceRegistry

NDN dependencies
  ndn-cxx, NFD, ndn-svs, NDNSD, NAC-ABE/OpenABE
```

## Core Runtime

The core runtime lives in `ndn-service-framework/`. Its primary public actors
are:

- `ServiceController`: distributes controller-signed permission responses.
- `ServiceProvider`: registers services, emits ACKs, handles selections, runs
  service handlers, publishes responses, and supports collaboration context.
- `ServiceUser`: prepares requests, collects ACKs, applies selection policy,
  sends selection messages, and receives responses.
- `ServiceContainer`: composes users, providers, and trusted local services in
  one process.
- `Stream`: app-neutral stream/session/chunk helpers for services that need a
  control invocation plus a named Data stream.

The standard runtime flow is:

```text
permission bootstrap
  user/provider fetch encrypted permissions from ServiceController

request
  user publishes RequestMessage under the V2 request namespace

ACK
  authorized providers reply with RequestAckMessage and provider token

selection
  user chooses one or more ACK candidates after the ACK window

response
  selected provider executes and publishes ResponseMessage
```

For live or long-running data streams, the service invocation should normally
carry control information such as start/stop/status and return a stream prefix.
The high-rate data path can then use signed named Data chunks under that prefix.
The reusable stream/session/chunk helpers live in `ndn-service-framework/Stream.*`;
application codecs, tensor formats, and GUI behavior stay in the application.

### Runtime Transfer Boundary

NDNSF has two reusable data-transfer surfaces, and applications should choose
between them by semantics rather than by byte size alone:

| Need | Runtime API | Naming model | Typical data |
| --- | --- | --- | --- |
| Continuous publication where new chunks keep arriving and late data may be less useful | `StreamInfo`, `StreamChunk`, stream buffers, stream fetch state | A stream prefix plus sequence numbers and session metadata | live UAV video, telemetry feeds, logs, status streams |
| Exact retrieval of one complete named object | Core large-data references, `publishLargeNamed(...)`, `fetchLarge(...)`, `fetchLargeExact(...)`, SegmentFetcher-style retrieval | A deterministic Data name or versioned object name | model artifacts, recorded video objects, manifests, DI tensor bundles |

Use the stream substrate when the application needs stream state: sequence
gaps, duplicate suppression, reordering, freshness, FEC metadata, or live
consumer buffering. Use the large-data path when the producer already knows the
object name and the consumer wants that exact object, even if the object is
large or segmented. A recorded video file is therefore large data; a live video
feed is a stream. A DI activation tensor with a planned dependency name is
large data; a future token-by-token LLM output feed would be a stream.

The two surfaces can be combined by an application, but one should not be used
as a vague replacement for the other. Stream chunks may carry application
frames or metadata for ongoing publication; they should not replace exact-name
SegmentFetcher retrieval for files, model chunks, or deterministic dependency
objects.

## Naming Direction

New code should use one unified `serviceName`:

```text
/ObjectDetection/YOLOv8
/FlightControl/Takeoff
/LLM/Llama3/Prefill
/HELLO
```

Avoid designing new APIs around split `ServiceName + FunctionName`.

## Invocation Modes

- **Normal service invocation**: full request, ACK, selection, and response
  path.
- **Targeted invocation**: known-provider low-latency path that still uses
  permission checks, request/response messages, replay protection, and
  one-time tokens. It skips ACK/selection only after Targeted token bootstrap.
- **Trusted local invocation**: same-process helper through
  `LocalServiceRegistry`. It is not a network mode and must not add new wire
  names, NAC-ABE attributes, or externally selectable request modes.

## Distributed Inference Native Path

The native DI path is under `NDNSF-DistributedInference/cpp/ndnsf-di/`.

Key boundaries:

- `NativeExecutionPlan`: service roles and dependency edges.
- `NativeProviderAssignment`: role-to-provider mapping.
- `ProviderRoleWorker`: prefetch inputs, run one role, publish outputs.
- `NativeProviderRuntime`: worker pool and role runner registry.
- `NativeProviderSession`: plan + assignment + dependency I/O + runners.
- `NativeProviderHandler`: adapts the native session to
  `ServiceProvider::CollaborationContext`.

This path is the performance direction for distributed inference. Python should
remain a planning, deployment, GUI, and experiment layer.

## Accepted Core/Application Ownership

The Spec 084 simplification program established one owner per concern:

- Core owns V2 normal/Targeted invocation, security, typed capability and
  operation envelopes, provider-owned leases, exact large-data transfer,
  continuous stream state, discovery facts, and provider-pair telemetry.
- DistributedInference owns model planning, fragments, caches, runtime
  lifecycle, dependency dataflow, and bounded replanning after lease rejection.
- DistributedRepo owns exact packet persistence, manifests, catalog, quorum,
  repair, and replica placement.
- UAV owns MAVLink, mission safety, operator authority, codecs, ROI, FEC policy,
  and ground-station workflow.

There is no generic Core advisory coordinator. Each DI user may plan
independently, but only a provider-owned fail-closed lease authorizes exclusive
execution. See [Core/App Boundary](ndnsf-core-app-boundary.md) for the complete
ownership and compatibility rules.

## Validation and failure ownership

Implementation checks, live local runs, immutable SIF replays, and Tiger jobs
are different evidence planes. A failure in an earlier plane invalidates any
later claim that depends on it; a passing focused check does not silently open
the next plane. The active Spec records the gate order, while the repository
[`failure-log.md`](failure-log.md) records the newest failed or unqualified
boundary and the next permitted action.
# NDNSF-DI Core/APP separation (Spec 111)

NDNSF-DI Core owns immutable execution contracts, eligibility, final proposal
validation, authenticated lease/certificate fencing, attempt epochs and result
authority. It does not import APP, Planner, SDK, ONNX, Qwen or operations code.
The process-local `DistributedInferenceEngine` is APP-owned and invokes ten
independently replaceable Python policies over an objective and one immutable
snapshot. Named Planner defaults use exactly the same SDK seams as external
packages. `RunnerAdapter` is an independent execution mechanism SPI and
`OptimizationObserver` is optional, idempotent and off-path.

Per-request placement is carried by immutable `AssignmentContext`; environment
variables are not placement authority. Deployment and durable request state is
owned by the APP RuntimeJournal. Model weights remain external artifact
references and are never included in owner wheels or container layers.
