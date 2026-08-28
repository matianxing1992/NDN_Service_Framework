# Generic Recovery Contract

## Retry

Timeout or Nack may reschedule only the same authenticated exact name while
the lifecycle generation, Mapping/session binding, schedulability, retry cap,
and horizon remain valid. Validation failure is terminal. Any suppression
records a generic reason and releases later eligible work.

## Multi-erasure recovery

Every repair symbol is independently signed and binds one stream/session/
mapping/group, its unique repair index, source names/cursors/lengths/digests,
and expiry. Recover only when missing sources do not exceed declared capacity,
enough independent valid repairs exist, and every reconstructed result matches
the declared length and digest. Otherwise emit no reconstructed bytes.

## Observability

Report initial/retry Payload Interests, initial/retry provider future hits,
cache/retention satisfaction, retry terminal reason, declared capacity, missing
count, recovery result, Mapping novelty, timeout, Nack, continuity, latency,
and coverage without logging payload contents.
