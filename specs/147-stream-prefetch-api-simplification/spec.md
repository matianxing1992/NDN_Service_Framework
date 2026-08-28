# Feature Specification: Stream/Prefetch API Simplification

**Feature Branch**: `[147-stream-prefetch-api-simplification]`  
**Created**: 2026-07-25  
**Status**: Complete  
**Input**: Define a smaller, symmetric C++/Python high-level Stream/Prefetch
API after Spec 146 closes. Freeze exact interfaces and before/after examples
without changing prefetch, Mapping, FEC, retry, or recovery algorithms.

## Context and Boundary

The current generic API is functional and validated. A Provider creates a
`LiveStreamDefinition`, obtains a `LiveStreamPublisher`, explicitly announces a
sample and manages its reservation, publishes at least one source, activates
the stream with readiness, and then continues publishing opaque source items.
A consumer builds `LiveStreamOpenOptions`,
opens a handle, and starts it separately. Python exposes equivalent primitives,
but some defaults and call shapes differ from C++.

This feature adds an optional convenience facade over those existing
primitives. The low-level `createLiveStream/create_live_stream` and
`openLiveStream/open_live_stream` APIs remain supported and authoritative.
The facade does not introduce a new wire format, controller, fetch state
machine, prefetch policy, FEC scheme, retry rule, workload selector, or
application-specific behavior.

Future announcement remains explicit after bootstrap because an Interest cannot
arrive before publication unless its exact future name has already been
committed. The facade hides reservation objects and common lifecycle plumbing;
it does not pretend that ordinary `publish()` alone can provide proactive
prefetch. Bootstrap is the one exception: no consumer descriptor exists yet,
so `start(initialSample)` announces and materializes the first sample before
activating the descriptor, matching the existing activation precondition.

## User Scenarios & Testing

### User Story 1 - Provider Publishes Through a Small Facade (Priority: P1)

As an application developer, I want to configure a generic stream once,
bootstrap it from one real sample, then announce future sample identities
without constructing names or reservations and publish opaque items by sample
ID so I can use Mapping and prefetch without reproducing Core lifecycle code.

**Why this priority**: Provider setup currently exposes many correctness
details that are identical across periodic applications and easy to misuse.

**Independent Test**: Run matching C++ and Python examples that create the same
configuration, bootstrap the same first sample, announce the same later
samples, and publish the same opaque items. Compare the resulting descriptor,
Mapping entries, exact names, signed source bytes, status, and failure behavior
with the low-level API.

**Acceptance Scenarios**:

1. **Given** a valid configuration, **when** a Provider creates a stream,
   **then** Provider identity, contract version, session identity, Mapping
   version, safe defaults, and canonical item names are derived consistently
   in C++ and Python.
2. **Given** a declared sample class and real initial opaque extent, **when**
   the application starts the stream, **then** the facade delegates to existing
   announce/publish/activate operations in that order and produces the same
   descriptor and packets as the low-level API.
3. **Given** an active stream, **when** the application announces a later
   sample and then publishes its real extent, **then** proactive Mapping remains
   explicit and delegates to the existing sample path.
4. **Given** duplicate, unknown, out-of-order, invalid-class, or post-stop use
   that the facade can validate before delegation, **when** an operation is
   attempted, **then** it fails before Mapping or payload publication.
5. **Given** an unexpected exception after an underlying publication operation
   has begun, **when** Core cannot promise transactional rollback, **then** the
   facade enters `Failed`, preserves the original exception, and permits no
   automatic retry, re-announcement, or claimed rollback.

---

### User Story 2 - Consumer Subscribes and Starts in One Call (Priority: P1)

As an application developer, I want one subscription call with the same
options in C++ and Python so I do not need to translate policy strings,
construct different option shapes, or remember a separate `start()` call.

**Why this priority**: A facade is not coherent if only Provider setup is
simplified or language defaults diverge.

**Independent Test**: Subscribe from C++ and Python to the same authenticated
descriptor using equivalent options. Verify that each call starts one existing
consumer handle, delivers the same verified items and provenance, reports the
same status, and stops idempotently.

**Acceptance Scenarios**:

1. **Given** a valid descriptor and item callback, **when**
   `subscribeStream/subscribe_stream` returns, **then** the underlying
   `LiveStreamConsumerHandle` is already active exactly once.
2. **Given** omitted policy and recovery options, **when** the descriptor is
   Mapping-v2 with declared FEC, **then** both languages select the same
   documented descriptor-aware defaults.
3. **Given** callback rejection, callback exception, invalid descriptor, or
   repeated stop, **when** the subscription processes it, **then** existing
   fail-closed callback and lifecycle semantics remain unchanged.

---

### User Story 3 - Advanced and Existing Applications Remain Stable (Priority: P1)

As an existing NDNSF application developer, I want the new facade to be
strictly additive so custom semantic names, manual reservation control, packet
feeds, and existing experiments continue using the present low-level API
without behavioral drift.

**Why this priority**: Simplification is not valuable if it silently changes a
validated protocol or forces migration.

**Independent Test**: Build and run existing low-level C++/Python Stream tests
and examples unchanged, then run parity tests proving that the facade calls
only existing primitives and adds no algorithm/workload branch.

**Acceptance Scenarios**:

1. **Given** an existing low-level caller, **when** the facade is added,
   **then** its source and runtime behavior remain compatible without
   deprecation or mandatory migration.
2. **Given** an application needing custom names or manual reservations,
   **when** it uses the low-level API, **then** no facade default or hidden
   state is imposed on it.
3. **Given** a neutrality review, **when** Core and bindings are scanned,
   **then** no UAV, telemetry, acoustic, audio, codec, or workload-specific
   branch has been added.

### Edge Cases

- `start()` receives an empty or invalid initial sample or is called twice.
- A sample ID is announced twice, published before announcement, published
  twice, or published after stop.
- Actual source count is below or above the announced class bound.
- An injected session identity collides with another live facade for the same
  Provider and base prefix in the current process.
- C++ and Python omit optional prefetch/recovery settings.
- An existing caller continues to use low-level custom names and reservations.

## Requirements

### Functional Requirements

- **FR-001**: The feature MUST add an optional facade and MUST retain the
  existing low-level C++ and Python APIs without removal, rename, or behavioral
  change.
- **FR-002**: C++ MUST expose
  `ServiceProvider::createStream(const StreamConfig&)`; Python MUST expose
  `ServiceProvider.create_stream(config: StreamConfig)` with the same semantic
  fields, defaults, validation, and lifecycle.
- **FR-003**: The returned `StreamPublisher` MUST expose only
  `start(initialSampleId, initialSampleClass, opaqueItems)`,
  `announce(sampleId, sampleClass)`, `publish(sampleId, opaqueItems)`,
  `status()`, and `stop()` in C++, with snake-case-equivalent Python methods
  where spelling differs.
- **FR-004**: `StreamConfig` MUST require a stream ID, base data prefix,
  positive sample period, and at least one sample-class profile. Provider
  identity MUST be inferred from `ServiceProvider`.
- **FR-005**: The facade MUST use Mapping contract v2. It MUST create a fresh,
  nonzero session epoch by default, use that epoch as the initial Mapping
  version, and append the same session-unique version component to the supplied
  base prefix. This preserves the existing resolver invariant that the final
  payload-prefix Version equals `mappingVersion`. An explicit
  session epoch MAY be injected only for deterministic tests or controlled
  restoration. Automatic generation MUST retry a process-local collision, and
  an injected epoch MUST be nonzero and MUST reject a collision with another
  live facade having the same Provider and base prefix. This is not a
  cross-process or persistent uniqueness registry.
- **FR-006**: Default exact names MUST be deterministic and generic:
  `<session-prefix>/sample/<sequence-number>/<source|repair>/<segment>`.
  Applications needing another naming scheme MUST use the existing low-level
  API rather than a workload option in the facade.
- **FR-007**: `announce()` MUST immediately delegate to the existing
  sample-atomic announcement path and retain its reservation privately by
  sample ID. Future announcement MUST remain a visible application action.
- **FR-008**: `start()` MUST require one valid, non-empty initial sample, wait
  for the existing publisher routes through a bounded readiness primitive,
  delegate exactly once to existing announce and publish operations, infer the
  safe join cursor from the materialized first source, use the configured
  sample period as readiness, delegate to the existing activation path, and
  return the authenticated `LiveStreamDescriptor`. It MUST reject execution on
  the Face I/O thread rather than block that event loop.
- **FR-009**: `publish()` MUST require an unpublished prior announcement,
  delegate actual-extent and publication work to the existing
  `publishSample/publish_sample` path, and remove the retained reservation only
  after successful publication.
- **FR-010**: A deterministic facade validation failure MUST occur before
  delegation. If an underlying bootstrap or publication call throws after it
  may have mutated Mapping or payload state, the facade MUST enter `Failed`,
  propagate the original error, and reject subsequent publication operations.
  It MUST NOT silently retry, re-announce, mutate the declared class, create a
  second reservation, or claim transactional rollback that Core does not
  provide.
- **FR-011**: C++ MUST expose
  `ServiceUser::subscribeStream(const LiveStreamDescriptor&,
  StreamSubscriptionOptions)`; Python MUST expose
  `ServiceUser.subscribe_stream(descriptor, options)` with equivalent options
  and defaults.
- **FR-012**: `subscribeStream/subscribe_stream` MUST construct the existing
  `LiveStreamOpenOptions`, call the existing open operation, start the returned
  handle exactly once before returning, and return that existing handle rather
  than a second consumer state machine.
- **FR-013**: `StreamSubscriptionOptions` MUST carry start position, optional
  prefetch policy, aggregate Interest limit, optional FEC-recovery setting,
  Interest lifetime, item callback, and optional status callback.
- **FR-014**: When the prefetch policy is omitted, both languages MUST select
  `AdaptiveSampleAtomic` for Mapping-v2 descriptors and `MappedPressure` for
  v1. When FEC recovery is omitted, both languages MUST enable it exactly when
  the authenticated descriptor declares FEC.
- **FR-015**: The item callback MUST retain the existing
  `VerifiedLiveStreamItem` and `LiveStreamItemAdmission` semantics. This
  feature MUST NOT add sample assembly, codec decoding, concealment, decryption
  policy, or a second callback queue.
- **FR-016**: High-level publisher state MUST be bounded by announced but not
  terminal sample IDs, be concurrency-safe, invoke no application callback
  while holding its bookkeeping lock, wait for route readiness only from an
  application thread with a configured finite startup timeout, and stop
  idempotently.
- **FR-017**: The facade MUST delegate Mapping, signing, validation, prefetch,
  FEC, retry, timeout, Nack, recovery, status, and packet production to existing
  implementations. No value or decision from Specs 144/146 may become a new
  algorithm default.
- **FR-018**: C++ and Python parity tests MUST compare field defaults, generated
  names, descriptor values, lifecycle transitions, errors, item delivery,
  callbacks, status, and stop behavior.
- **FR-019**: Documentation MUST contain paste-ready C++ and Python before/after
  examples and explicitly explain that `announce()` remains necessary for
  proactive prefetch.
- **FR-020**: A post-implementation CodeGraph and source audit MUST find no
  workload-specific Core/binding branch and no change to frozen Specs 144/146
  results.

### Key Entities

- **StreamConfig**: Minimal application configuration plus an optional nested
  advanced resource-cap block; it deterministically produces one existing
  `LiveStreamDefinition`.
- **StreamPublisher**: Thin Provider-side facade holding only the underlying
  publisher, lifecycle, and bounded sample-ID-to-reservation table.
- **StreamSubscriptionOptions**: Language-symmetric input translated once into
  existing open options.
- **LiveStreamDescriptor / LiveStreamConsumerHandle**: Existing authenticated
  descriptor and consumer handle reused unchanged.

## Success Criteria

### Measurable Outcomes

- **SC-001**: The normal Provider example is at most five lifecycle calls after
  configuration: create, start-with-first-sample, announce-next, publish-next,
  stop; it exposes no
  reservation, readiness, name-factory, contract-version, Provider-identity,
  session-epoch, or Mapping-version plumbing.
- **SC-002**: The normal consumer example uses one subscribe call and no
  separate start call in both languages.
- **SC-003**: Contract tests prove C++/Python parity for every public facade
  field, default, method, generated name, and failure category.
- **SC-004**: Golden parity tests show the facade and low-level API produce
  identical descriptors, Mapping semantics, signed packet names/content, item
  callbacks, status counters, and stop outcomes for matched inputs.
- **SC-005**: All existing Stream, binding, example, security, and Spec 146
  focused regressions pass without modifying their expected behavior.
- **SC-006**: A code-aware audit finds zero new prefetch/FEC/retry/recovery
  algorithm branch and zero application/workload selector in Core or bindings.
- **SC-007**: Frozen Spec 144 and Spec 146 canonical artifact hashes remain
  unchanged; no network matrix is required for the additive facade unless
  parity or integration tests expose a real runtime gap.

## Assumptions

- Applications can provide stable numeric sample IDs and a class before the
  corresponding sample is produced.
- The default canonical name is sufficient for the common path; callers with a
  custom semantic-name contract remain on the low-level API.
- The implemented facade remains additive; callers needing custom names or
  manual reservations continue to use the low-level API.

## Out of Scope

- Any prefetch, Mapping, FEC, retry, timeout, Nack, validation, or recovery
  algorithm change.
- Sample assembly, playback, codec, sensor, UAV, telemetry, or audio policy.
- Removal or deprecation of the existing low-level API.
- Repetition or modification of frozen Spec 144 or Spec 146 campaigns.
- A new MiniNDN performance matrix solely to test syntax-level convenience.
