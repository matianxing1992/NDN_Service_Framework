# B189-4 Resource and Drain Evidence

**Status**: NOT_STARTED

No Spec189 resource-guard run has been started. Historical Spec184/188 Qwen runs remain historical resource boundaries and are not reused as this candidate's result.

The current Qwen runner only starts, waits for markers and terminates child
processes; it does not yet sample RSS, MemAvailable, swap, Repo resident bytes,
materialization bytes or process-group drain. The r08 stale 1.5-GB publication
residue is therefore a recorded failure boundary, not T008 resource evidence.

## Five-lane coverage

All lanes are `gap` pending C++ lease/runner counters, host sampling and deterministic process-group drain.

## Closure decision

`OPEN_FOR_NEXT_BATCH` with trigger: add the C++ ownership counters and the
maintained Python sampler/guard, then run it around a complete or classified
B189-3 execution. A cleanup `finally` block alone is insufficient.
