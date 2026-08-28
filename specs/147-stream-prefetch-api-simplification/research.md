# Research: Stream/Prefetch API Simplification

## Decision 1 - Add a facade instead of replacing the engine

**Decision**: Keep `LiveStreamPublisher`, `LiveStreamDescriptor`,
`LiveStreamOpenOptions`, and `LiveStreamConsumerHandle` authoritative. Add
`StreamPublisher` and convenience entry points that delegate to them.

**Rationale**: CodeGraph confirms the current implementation is wired through
`ServiceProvider::createLiveStream` and `ServiceUser::openLiveStream`, with
native and Python tests covering the validated protocol. Replacing these
objects would duplicate state and invalidate existing applications.

**Alternatives considered**:

- Rename/remove the old API: rejected because it forces migration without user
  value.
- Create a second Stream protocol: rejected as unnecessary and unsafe.

## Decision 2 - Keep future announcement explicit

**Decision**: The facade bootstraps with
`start(initialSampleId, initialSampleClass, opaqueItems)`, then retains
`announce(sampleId, sampleClass)` before
`publish(sampleId, opaqueItems)` for every later sample.

**Rationale**: The existing `activate()` requires its safe-join source to be
materialized, so `announce -> start -> publish` is not a valid delegate order.
Before the first descriptor exists there can be no consumer prefetch, making
the bootstrap combination safe. After bootstrap, the existing sample-atomic
mechanism must commit exact future names through Mapping before publication.
Hiding those later announcements inside `publish()` would occur too late for
proactive Interests and would misrepresent the feature.

**Alternatives considered**:

- One ordinary `publish()` call with no prior announcement: rejected because it
  cannot provide future-name prefetch.
- Background prediction of application classes: rejected because it introduces
  application policy and a new prefetch algorithm.

## Decision 3 - Generate only one generic default name

**Decision**: The facade generates one session-versioned, sample/source-or-
repair/segment name layout. Custom semantic names remain a low-level feature.

**Rationale**: Accepting a name callback would reintroduce the reservation/name
plumbing the facade is intended to hide. A workload-name enum would violate
Core neutrality.

## Decision 4 - Reuse descriptor and consumer handle

**Decision**: `start()` returns the existing `LiveStreamDescriptor`, and
`subscribeStream/subscribe_stream` returns the already-started existing
`LiveStreamConsumerHandle`.

**Rationale**: New descriptor or subscription state machines would add
translation, security, and lifecycle drift without simplifying the caller.

## Decision 5 - Freeze descriptor-aware defaults symmetrically

**Decision**: Omitted policy selects `AdaptiveSampleAtomic` for Mapping v2 and
`MappedPressure` for v1. Omitted FEC recovery follows the descriptor's FEC
declaration in both languages.

**Rationale**: Python already selects the descriptor-aware prefetch default,
while the low-level C++ struct has a static default. A new symmetric facade
must document one behavior while leaving low-level defaults untouched.

## Decision 6 - No new network experiment

**Decision**: Validate the facade with golden delegate parity, native/Python
contract tests, existing integration examples, and current regressions.

**Rationale**: The facade must not change network behavior. A new performance
matrix would add cost without testing new protocol behavior. If parity tests
expose a runtime difference, that difference blocks the facade and requires a
separate diagnostic decision.

## Decision 7 - Bounded route readiness, never Face-thread blocking

**Decision**: Bootstrap waits through a new internal bounded readiness
primitive on the existing publisher. It is callable only from an application
thread while the Face loop runs independently; Face-thread invocation fails
immediately.

**Rationale**: `createLiveStream()` begins asynchronous route registration and
`activate()` requires both routes ready. A convenience API that ignores this
race is unreliable, while a wait on the Face thread deadlocks the callbacks
that establish readiness. This is lifecycle plumbing only and does not change
network fetching or prefetch policy.

## Decision 8 - Fail closed instead of promising transactional rollback

**Decision**: Validate deterministic facade errors before delegation. If an
underlying mutating call throws, move the facade to `Failed`, propagate the
error, and allow no automatic retry or re-announcement.

**Rationale**: The existing `publishSample()` may publish a base group before
processing a continuation or predictor result. The facade cannot honestly
promise an atomic rollback across signed Mapping and payload Data without
changing Core behavior, which is out of scope.

## Decision 9 - Session collision checking is process-local

**Decision**: Track live facade sessions by Provider, base prefix, and epoch in
the current process. Retry automatically generated collisions and reject
nonzero injected epochs that collide with a live tuple.

**Rationale**: The facade can prevent accidental same-process reuse without
adding a controller or persistent coordination protocol. Fresh random epochs
make restart collisions improbable, but the API does not claim distributed
uniqueness.
