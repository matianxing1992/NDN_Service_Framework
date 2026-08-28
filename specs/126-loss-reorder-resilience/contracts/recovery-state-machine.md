# Recovery State-Machine Contract

## Invariants

1. `(sessionEpoch, cursor)` identifies one local lifecycle.
2. An attempt may produce one network terminal callback: Data, Nack, or timeout.
3. A cursor may produce one application terminal outcome.
4. Validation, recovery, retry, and stop are fenced by session and generation.
5. Retries express the same exact semantic name.
6. `Delivered`, `Recovered`, `Skipped`, `TerminalUnproduced`, and `Stopped` are
   terminal and reject later mutation.
7. Predictor observation is group-idempotent and accepts authenticated actual
   extent only once.

## Recovery

- Zero missing sources: the repair is redundant and cannot change delivered
  content.
- Exactly one missing source plus one valid repair: reconstruct byte-for-byte,
  pass normal admission once, and mark `Recovered`.
- More than one missing source: remain pending until deadline/budget, then fail
  closed and advance with an explicit terminal reason.
- Late signed source after recovery: count `late-terminal`; do not call the APP.

## Required deterministic permutations

- source then repair; repair then source;
- timeout then retry Data; Nack then retry Data;
- Data reception then slow validation while siblings complete;
- recovery then late source; source delivery then late repair;
- stop/session change during pending, validation, and recovery;
- terminal-unproduced suffix mixed with delayed real source;
- invalid signature/name/extent/digest at every arrival rank.
