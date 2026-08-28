# Research Decisions: Bidirectional Capability Comparison

## Decision 1: Test public usage models, not one artificial adapter

The baseline NDN-SVS chat-pubsub example runs `face.processEvents()` in a
separate thread and calls synchronous `publish()` from the application main
thread. The latest header documents `publishAsync()` as preparing/staging a
publication and deferring ordered advertisement to the Face io_context.
Therefore each frozen subject must be exercised through its own public usage
model without a harness-owned publication queue.

## Decision 2: Treat synchronous concurrency failure as subject evidence

An earlier direct high-rate probe exposed an AddressSanitizer double-free in
scheduler cancellation despite the baseline example's thread-safety claim.
Moving `publish()` onto Face hides that boundary and changes the usage model.
The corrected harness records any recurrence as a subject failure with one
terminal receipt.

## Decision 3: Both peers publish and receive

A one-way publisher/subscriber topology measures unequal roles. The requested
PubSub workload uses symmetric peers, each with one local publication stream and
one remote subscription stream. Directional analysis prevents one healthy
direction from hiding failure in the other.

## Decision 4: Use the five baseline formal rates as the tested ceiling search

The fixed 200/400/600/800/1000 per-peer sweep already locates the highest
sustainable point in the requested grid. Extra formal calibration cells would
violate the ten-cell budget. Absolute deadlines give approximate synchronous
pacing; achieved returns and remote delivery decide capacity.

## Decision 5: Separate local return from remote success

Synchronous `publish()` may return faster than Face can advertise and serve
the stream. `publishAsync()` returns after preparation/staging, not remote
delivery. The experiment therefore reports API-entered, API-returned, and
remote-delivered as separate quantities and defines sustainable capacity using
both directions' delivery.

## Experiment Variables And Confounds

- Independent variable: immutable capability subject commit.
- Controlled offered variable: per-peer target rate.
- Dependent measures: per-peer API entry/return rate, direction-specific
  delivery ratio, latency distribution, duplicates, reorder, resource use.
- Controls: topology, payload, wire profile, timers, security, warmup/window,
  build patch, CPU allowance, cell order.
- Known limitations: one observation per rate; complete commit-bundle effect;
  no replication-based uncertainty interval; capacity ceiling is limited to the
  tested grid.

