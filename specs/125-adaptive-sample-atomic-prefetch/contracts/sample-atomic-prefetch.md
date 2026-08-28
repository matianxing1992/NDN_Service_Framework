# Contract: Sample-Atomic Prefetch

## Mapping v2

Every non-tombstone Mapping v2 entry MUST encode exactly one semantic Name and
one canonical group binding. An adaptive descriptor and resolver MUST require
contract version 2. Required group fields are ordered and minimally encoded:

```text
GroupId                  non-negative integer
ClassId                  bounded opaque UTF-8 string
GroupItemIndex           non-negative integer
PredictedSourceItems     positive non-negative integer
PredictedRepairItems     non-negative integer
Name                     canonical NDN Name
```

The entry is invalid when:

- `GroupItemIndex >= PredictedSourceItems + PredictedRepairItems`;
- a count exceeds the session/class/atomic group cap;
- the same group tuple changes across entries or Mapping blocks;
- indexes are missing, duplicated, reordered, or overlap another group;
- the first joinable cursor is not a group boundary;
- canonical re-encoding differs from received wire.

Resolver admission remains all-or-nothing for a block and the existing digest
chain remains mandatory.

An explicitly pinned v1 descriptor may use only the legacy manual policy and v1
entry grammar. It cannot admit a v2 block or expose sample-atomic status. An
explicitly pinned v2 descriptor cannot admit v1.

## Predictor

- State key: `(sessionEpoch, classId)`.
- Training input: authenticated complete actual source extent only.
- Capacity: fixed maximum classes and observations/class.
- Cold start: declared class seed.
- Output before any observation: `seed`.
- Output after observation: `clamp(1, max(last H same-class actual counts) + margin, hardMax)`;
  the cold-start seed is not a permanent lower bound.
- Diagnostics: predicted/actual, under/over count and magnitude, warm/cold.

Golden vectors MUST freeze FIFO eviction, integer margin, reset, and cap
behavior identically for C++ and Python.

## Public API

Provider high-level operations:

```text
declare sample classes and sample period in LiveStreamDefinition
announceSample(sampleId, classId, semanticNameFactory) -> SampleReservation
prepareSampleExtent(SampleReservation, actualSourceItems) -> exact source reservations
publishSample(SampleReservation, opaqueSourceItems)
```

`prepareSampleExtent` is optional for content whose protection does not bind the
final Data name. Name-bound AEAD applications (including UAV video) MUST call it
after packetization and before encryption; it accepts the APP-observed actual
count, never an APP-selected prediction. `publishSample` calls the same operation
idempotently as a convenience and rejects an extent that changes after preparation.

Consumer high-level operations:

```text
openLiveStream(descriptor, AdaptiveSampleAtomic options) -> ConsumerHandle
ConsumerHandle.start/status/stop
```

The Provider APP cannot pass a predicted window to `announceSample`; the
consumer APP cannot pass a key/delta window to `openLiveStream`. Both may read
bounded status. C++ and Python must produce identical Mapping and prediction
golden vectors.

## Scheduling

For each next mapped group, the scheduler MUST either:

1. issue every not-yet-complete predicted source and selected repair Interest
   that fits the whole-group contract; or
2. issue none of that group and report a bounded deferral reason.

It MUST NOT make progress by issuing only a group prefix. Retransmissions of an
already-started group are a separate recovery action and do not violate this
initial-issuance rule.

## Variable FEC

An XOR-one-repair session declares `maxSourceItems`, not an exact source count.
Each group declares its real `actualSourceItems` in `[1, maxSourceItems]` and
contains zero or one repair. With one source and one repair, the repair is a
recoverable copy of the opaque source. Recovery binds the group ID, actual
source list, lengths, and digests. Empty padding sources are invalid.

## Security

- Group/class/count metadata is Provider-signed through Mapping and group Data.
- Consumer state changes only after normal Validator success and expected
  Provider identity checks.
- APP ciphertext remains opaque; Core does not decrypt to discover boundaries.
- Conflicting or oversized metadata fails closed without partial resolver or
  predictor mutation.
- Class IDs are scheduling hints, not authorization attributes.
