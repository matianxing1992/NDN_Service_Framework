# Data Model: NDNSF Stream Latency Attribution and Continuity

## StreamCorrelationIdentity

- `streamId`: stable stream identifier
- `sessionEpoch`: anti-replay session generation
- `publicationCursor`: immutable Mapping slot for a source item
- `sourceId`: canonical correlation token derived from the immutable source binding
- `mediaSequence`: optional APP decoder order
- `frameId`: optional APP input-group/frame identifier
- `segmentIndex`: optional source shard within the group
- `outputOrdinal`: optional decoder output position; never used to invent a source binding

Validation: stream/session/source identity are required for cross-stage joins. Numeric sequence equality alone is never sufficient.

## StreamProgressSnapshot

- verified resolver frontiers
- scheduler next cursor and fetcher reservation frontier
- Mapping/payload/retry in-flight counts
- provider-produced and consumer-delivered frontiers where known
- last progress monotonic time
- lifecycle state and reason

Invariant: all frontiers are monotonic within one session; scheduler bounds cannot lag an accepted resolver frontier after the acceptance callback returns.

## LatencyEvent

- correlation identity
- role and stage
- monotonic timestamp and clock-domain identifier
- startup/warmup/steady classification
- sampled flag and metadata

Invariant: an interval exists only when both events share a compatible identity and clock authority.

## LatencyDistribution

- metric name with explicit endpoints
- phase (`startup`, `warmup`, `steady`)
- sample count, p50, p95, p99
- excluded/missing/invalid counts and reasons
- clock uncertainty when cross-clock

## ExperimentCell

- unique candidate identity
- frozen command, environment, topology, workload, logging, duration, and warmup
- correctness and continuity outcome
- Mapping/payload/future-hit/unnecessary-Interest counters
- decoded frame and correlation counters
- CPU, memory, queue/PIT high-water marks
- latency distributions and evidence paths

State: `planned -> running -> pass|fail|invalid`. A terminal cell is never silently rerun under the same identity.
