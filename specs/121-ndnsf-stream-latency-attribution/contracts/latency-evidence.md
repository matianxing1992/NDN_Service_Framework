# Latency Evidence Contract

## Source identity

Every source-stage event carries one canonical correlation name:

```text
/<trace-prefix>/<stream_id>/<session_epoch>/<publication_cursor>
```

The correlation name is the source identity. An optional `source_id` digest may
repeat that binding, but a bare numeric cursor is never an identity.

UAV-owned optional fields:

```text
media_sequence, frame_id, segment_index, output_ordinal
```

The correlation name binds to the immutable Mapping entry. Repair events use a distinct repair identity and cannot enter source latency distributions.

## Required stage vocabulary

Provider local: `encoded-output-ready`, `group-ready`, `protection-complete`, `signed-and-materialized`, `data-put`.

Consumer local: `data-received`, `signature-validated`, `decrypted`, `reorder-ready`, `decoder-input`, `decoder-first-output`, `decoder-output`, `gui-delivered`.

## Join rules

1. Same `stream_id`, `session_epoch`, and `source_id` are mandatory.
2. Stages must have compatible roles and cardinality.
3. Same-clock subtraction requires the same monotonic clock domain.
4. Cross-clock subtraction requires offset and uncertainty; otherwise it is unavailable.
5. Startup and steady samples never share one percentile distribution.
6. Missing, sampled-out, ambiguous, and one-to-many unmatched records are counted, not guessed.

## Progress contract

After accepting a Mapping block, the resolver frontier and fetcher legal frontier must agree before payload rescheduling. ACTIVE with provider progress but no consumer progress longer than two measured production periods must emit an explicit starvation reason.

## Compatibility

This contract adds evidence metadata only. It does not change Mapping/source/repair wire formats, exact semantic Data names, signatures, encryption, or the public Stream open/publish callbacks.
