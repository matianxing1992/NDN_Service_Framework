# Prefetch and Retention Contract

1. Mapping predicts an immutable semantic Data name; it does not prove that its payload is currently retained.
2. A Provider holds an Interest only when the mapped cursor has never been materialized.
3. A previously materialized but evicted name is never counted as an eligible future Interest.
4. The consumer's emitted work must be a subset of the current adaptive decision's cursor range and budgets.
5. The decision's Interest lifetime is authoritative and remains bounded; the consumer must not derive a longer lifetime from the session-start checkpoint.
6. Under `MappedLiveFutureOn`, expiry of that lifetime terminally skips the cursor because its live usefulness deadline has passed. Beginning/recording playback retains retries. No alternate payload is published under an immutable evicted Data name.
7. Timeout, stale skip, and live rejoin do not bypass Data signature validation, Mapping validation, AEAD authentication, replay rejection, or FEC validation.
8. UAV frame assembly holds at most one cursor-ordered access unit; a later frame explicitly drops an incomplete predecessor and can then progress.
