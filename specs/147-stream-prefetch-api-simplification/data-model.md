# Data Model: Stream/Prefetch API Simplification

## StreamConfig

Required:

- `streamId / stream_id`: non-empty stable logical ID;
- `dataPrefix / data_prefix`: non-empty base NDN name;
- `samplePeriodMs / sample_period_ms`: finite positive period;
- `sampleClasses / sample_classes`: at least one existing
  `SampleClassProfile`.

Optional:

- `fec`: existing `LiveStreamFecOptions`, default none;
- `sessionEpoch / session_epoch`: test/restoration injection only;
- `advanced`: existing capacity/resource limits plus a finite
  `startupTimeoutMs / startup_timeout_ms`, with documented defaults.

Derived:

- Provider identity from `ServiceProvider`;
- Mapping contract v2;
- fresh nonzero session epoch when not injected;
- Mapping version equal to the session epoch, preserving the existing resolver
  invariant that the semantic payload prefix's final Version component equals
  `mappingVersion`;
- session-versioned semantic data prefix;
- existing `LiveStreamDefinition`.

## StreamPublisher

State:

```text
Created -> Bootstrapping -> Active -> Stopped
                              \-> Failed
```

Owned data:

- one existing `LiveStreamPublisher`;
- bootstrap source cursor;
- bounded map from sample ID to existing
  `LiveStreamSampleReservation`;
- lifecycle and mutex.

Rules:

- `start` waits for route readiness from an application thread, then announces
  and materializes the first sample before activation;
- `announce` is valid after activation but not before bootstrap or after
  stop/failure;
- sample ID and class become immutable after successful announcement;
- `start` is exactly once and requires a valid non-empty initial sample;
- `publish` requires an outstanding announcement;
- successful publication retires its facade reservation;
- deterministic facade validation fails before delegating to Core;
- an exception after an underlying mutating call begins transitions the facade
  to `Failed`, preserves the original error, and rejects later publication;
- the facade never silently retries, re-announces, or claims rollback for
  Mapping or payload state that Core may already have committed.

## Session Identity Registry

The facade owns a process-local registry keyed by
`(Provider identity, base data prefix, session epoch)`.

- automatic generation retries a live collision and always produces nonzero;
- injected epochs are accepted only when nonzero and not live under the same
  Provider/base-prefix tuple;
- `stop()` releases only the live registry claim;
- the registry does not claim cross-process or post-restart uniqueness.

## StreamSubscriptionOptions

Fields:

- start position, default latest;
- optional prefetch policy, default descriptor-aware;
- aggregate Interest limit, default 64;
- optional FEC recovery, default descriptor-aware;
- Interest lifetime, default 500 ms;
- required item callback;
- optional status callback.

It translates once into existing `LiveStreamOpenOptions`.

## Reused Existing Entities

- `LiveStreamDescriptor`: returned unchanged by Provider activation.
- `VerifiedLiveStreamItem`: delivered unchanged to the application.
- `LiveStreamItemAdmission`: callback decision unchanged.
- `LiveStreamConsumerHandle`: opened, started once, and returned unchanged.
