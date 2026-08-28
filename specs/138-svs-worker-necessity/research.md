# Research Decisions

## Decision: Preserve The Exact Existing Subject

**Decision**: Reuse and re-hash the current `6bb34545` binary from
`build/spec137-four-core`; do not rebuild or alter the measurement patch unless
hash or linkage verification fails.

**Rationale**: This is the strongest same-binary comparison and removes build
variation. The active NDN-SVS checkout is still exactly `6bb34545`.

**Alternatives considered**: Rebuild under a new Spec path. Rejected because it
adds no causal value while the frozen artifact is intact.

## Decision: Select Pressure With The Control Arm Only

**Decision**: Use a descending preregistered rate ladder and select the first
valid Face-inline pressure point before running the worker qualification.

**Rationale**: A fixed 60 pps cell did not meaningfully stress Face production.
Selecting from both-arm outcomes would bias the experiment toward a favorable
worker result.

**Alternatives considered**: Fix 1000 pps without qualification; search for the
largest observed treatment effect. The former risks an invalid common-path
rate and the latter is outcome shopping.

## Decision: Gate Host Contention Before Cell Start

**Decision**: Record per-core utilization and wait boundedly for the frozen CPU
map to be quiescent before starting each cell.

**Rationale**: Spec 137 pair 1 lost releases in both modes and recorded tens of
seconds of skipped heartbeat time, while later identical cells were exact.
That is common host starvation, not a treatment effect.

**Alternatives considered**: Ignore pair 1; rerun it; require eight CPUs. All
are rejected: the receipt is immutable, rerunning is invalid, and one worker
does not need eight CPUs.

## Decision: Use A Local Necessity Claim

**Decision**: “Necessary” means necessary to meet the registered Face
responsiveness boundary at the selected workload without delivery harm.

**Rationale**: No finite benchmark can prove universal necessity. A bounded,
falsifiable systems claim is defensible.

**Alternatives considered**: Claim universal necessity from moved CPU work.
Rejected because offload can be neutral at low load.
