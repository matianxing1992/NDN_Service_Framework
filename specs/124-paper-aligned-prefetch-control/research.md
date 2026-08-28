# Research: Paper-Aligned Prefetch Control

## Source

Gusev et al., *Real-Time Streaming Data Delivery over Named Data Networking* (IEICE, 2016): <https://irl.cs.ucla.edu/data/files/papers/gusev2016realtime.pdf>

## Verified mechanisms

- Pipeline demand is derived from estimated Data retrieval delay after removing data-generation delay, not from the complete wait experienced by an early Interest.
- Chasing increases the Interest pipeline; adjustment withholds Interests; after a change the algorithm waits for a detection period before another change.
- When withholding causes stale Data, the previous pipeline value is restored as the fetching value.
- Detection compares two consecutive non-overlapping arrival-period windows and requires repeated stable outcomes.
- The paper's frame estimator, jitter buffer, retransmission checkpoint, and bitrate challenge are useful references but are not all Core-prefetch requirements.

## Project decisions

### Decision: do not add per-consumer wait metadata to Data

NDN Data is immutable and cacheable. Producer wait is Interest-specific, so putting it into the signed Data content would either make one cached object carry the wrong wait for other consumers or require a new per-request response identity. Use direct known-produced observations. During live-edge search, an ahead-mapped effective delay may lower an overestimate but cannot raise network RTT; after stable fetching makes generation wait small, normal adaptation resumes.

### Decision: restore, do not rechase, after over-adjustment

This follows the paper's control intent and prevents avoidable doubling oscillation.

### Decision: keep security and semantic-name extensions

The paper does not provide the complete trust/encryption contract NDNSF needs. Signed Mapping, signed original semantic Data names, validation, optional AES-GCM/FEC, and bounded state remain project requirements.
