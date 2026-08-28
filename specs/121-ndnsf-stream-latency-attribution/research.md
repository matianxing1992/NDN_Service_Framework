# Research: NDNSF Stream Latency Attribution and Continuity

## Decision 1: Treat the current result as a measurement/correctness failure first

**Decision**: Do not tune the prefetch window from the existing p95. Repair continuous scheduling and correlation first.

**Rationale**: Provider production reached cursor 304 while the consumer stopped after 30 payload Interests. The reported one-second distribution also mixes publication cursors, media sequences, decoder cold start, and one-to-many frame output.

**Alternatives considered**: Increase window/lifetime immediately; embed Data in Mapping; reduce FEC group size. All are rejected before a trustworthy baseline exists.

## Decision 2: Resolver is the Mapping authority

**Decision**: Propagate resolver frontiers into the adaptive fetcher after each accepted Mapping block.

**Rationale**: Duplicating Mapping validation in the fetcher would create two authorities. The fetcher needs only monotonic bounds for legal scheduling.

**Alternatives considered**: Remove the fetcher's reservation guard; periodically poll provider state. Both weaken correctness or add avoidable Interests.

## Decision 3: Separate publication and media ordering

**Decision**: Retain publication cursor for Core and add explicit APP media fields plus a stable source correlation ID.

**Rationale**: Publication order includes repair items; decoder order contains source shards only. Reusing one integer for both caused synthetic gaps and false trace joins.

**Alternatives considered**: Force UAV media sequence to equal publication cursor. Rejected because optional FEC repairs legitimately occupy publication cursors.

## Decision 4: Measure one-way behavior causally, not by unsupported clock subtraction

**Decision**: Use provider-local and consumer-local intervals plus causal evidence that Interest preceded production. Cross-host one-way time requires bounded clock uncertainty.

**Rationale**: Pre-issued exact-name Interests prove removal of the extra Mapping RTT without inventing precise one-way latency from unrelated clocks.

**Alternatives considered**: Assume clocks are synchronized in every deployment. Rejected; only MiniNDN's shared host provides that convenience.

## Decision 5: Separate decoder startup from steady state

**Decision**: Report join-to-first-frame, first-input-to-first-output, and steady distributions independently.

**Rationale**: Existing logs show about 205 ms synthetic reorder wait, about 700 ms FFmpeg cold start, and later sampled output delays around 10–12 ms.

**Alternatives considered**: Drop the first sample only. Rejected because startup can span several frames and one input group can emit multiple frames.

## Decision 6: Preserve the paper-inspired prefetch principle

**Decision**: Keep names-only Mapping ahead of production and exact semantic-name future Interests. Optimize only consumer lead/window or producer batching behavior supported by corrected evidence.

**Rationale**: The observed eligible future-hit ratio was already 25/25 before the stream stalled, so the conceptual prefetch path works; its horizon handoff is broken.

**Alternatives considered**: Data-in-Data. Rejected because it adds wire/security/cache complexity and is not needed to fix continuity.

## Decision 7: Bound encoder-pipe and packetization wait

**Decision**: Use POSIX pipe reads and a bounded partial-packet flush, with the old stdio-batched path available only as an explicit rollback.

**Rationale**: The corrected baseline showed 917 ms from first decoder input to first output and 1066 ms p99 output gaps, while network, validation, decryption, reorder, and GUI stages were tens of milliseconds or less. The provider's blocking 8192-byte stdio read was outside the previous clock and explains the burst pattern.

**Alternatives considered**: Increase the prefetch window. Rejected because future-hit ratio was already 100% and cannot reduce time spent waiting for encoder bytes.
