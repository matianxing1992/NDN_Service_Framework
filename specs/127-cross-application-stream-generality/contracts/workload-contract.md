# Contract: Application-Neutral Workloads

## Provider boundary

Both fixtures use only the existing Mapping v2 operations:

```text
declare opaque sample classes
announceSample(sampleId, classId, semanticNameFactory)
prepareSampleExtent(reservation, actualSourceItems)
publishSample(reservation, opaqueSourceItems)
```

The fixture may know cadence, byte generation, and actual extent. It may not
pass a packet window, predicted size, codec/media/sensor semantics, or security
exception. Semantic names identify workload and sample/segment only; Core does
not branch on those components.

## Consumer boundary

The consumer uses the default Mapping v2 adaptive sample-atomic policy. It
accepts authenticated opaque items, reconstructs complete sample receipts from
signed group metadata, and returns the ordinary admission result. It may not
parse payload contents to control fetching.

## Periodic fixture

- 100 ms period; 256-byte deterministic opaque source.
- One source item, no repair, one opaque class.
- Pauses/resumes and stop fencing are deterministic-test inputs only; the live
  workload cadence remains fixed.

## Variable fixture

- 100 ms period; source segments up to 4096 bytes.
- Four opaque cap classes: 1, 2, 4, 8 sources.
- Balanced seeded order; actual extents follow the plan's within-class sets.
- One XOR repair per sample; repair bytes remain opaque Core content.

## Prohibited coupling

No Spec 127 edit may add any of these to `Stream.{hpp,cpp}` or bindings:

```text
sensor, telemetry field, UAV, drone, camera, codec, H264/H265,
key frame, delta frame, workload ID, benchmark name, application service name
```

Existing historical identifiers are not attributed to Spec 127; the audit
uses scoped diff plus call-path review rather than a raw repository-wide count.
