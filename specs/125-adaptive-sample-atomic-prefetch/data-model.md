# Data Model: Adaptive Sample-Atomic Prefetch

## `SampleClassProfile`

```text
classId                 opaque bounded string
seedSourceItems         conservative cold-start count
hardMaxSourceItems      absolute accepted bound
historyCapacity         bounded observation count
safetyMarginItems       additive conservative margin
observations[]          authenticated actual source counts
predictedSourceItems    current bounded prediction
underpredictions        count and missing-item total
overpredictions         count and excess-item total
```

Invariant: profiles are session-scoped; `seed <= prediction <= hardMax`; an
observation from one class never changes another profile.

## `StreamSampleGroupBinding`

```text
groupId                 unique within session
classId                 opaque predictor class
itemIndex               cursor's index in predicted group
predictedSourceItems    predicted source extent M
predictedRepairItems    selected repair extent R
originalName            semantic NDN Data name
cursor                  derived from Mapping block and slot
```

Invariant: indexes are contiguous `[0, M+R)`; tuple is canonical and identical
for every entry in the group; names/cursors are never reused.

## `SampleExtentObservation`

```text
sessionEpoch
groupId
classId
predictedSourceItems
actualSourceItems
actualRepairItems
authenticated
complete
observedAtMs
```

Only authenticated and complete observations train the predictor. Recovered
payload may count only when its group boundary remains unambiguous.

## `SampleAtomicFetchDecision`

```text
sampleDemand
predictedPacketDemand
effectivePacketWindow
admittedGroups[]
deferredGroup
atomicExpansions
atomicDeferrals
capacityReason
```

Invariant: `effectivePacketWindow` ends at the last admitted group boundary.
No admitted group is partially scheduled.

## `FutureSampleAnnouncement`

```text
sampleId                 APP-defined monotonic sample identity
classId                  expected opaque class
announcedAtMs            producer monotonic time
expectedProductionMs     optional timing hint from fixed sample period
nameFactory              APP callback producing semantic names by item/kind
reservation              Core-owned predicted group binding
preparedExtent           idempotent real-count reconciliation before name-bound AEAD
```

The APP never supplies `predictedSourceItems` or a packet window. A class
mismatch at publication is explicit and cannot mutate the signed reservation.

## State transitions

```text
class cold -> warm              enough valid observations
reserved -> mapped             signed Mapping v2 committed
mapped -> partially produced   first authentic item reveals actual extent
partially produced -> complete all actual sources/repairs admitted
mapped -> terminal suffix       N < M; unused reserved names never reused
mapped -> continuation          N > M; tail mapped/fetched without prefetch credit
```
