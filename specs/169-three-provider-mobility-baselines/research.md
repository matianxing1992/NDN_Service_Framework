# Research Decisions

## Decision 1: Retrospective baseline remediation

**Decision**: Keep the historical NDNSF mobility values frozen and rerun only
three-Provider NSC and gRPC baselines.

**Rationale**: The original NDNSF raw trace and traffic/latency evidence are not
retained, so a new trace-paired three-system campaign cannot be created without
rerunning NDNSF.

**Alternative considered**: Calling the reconstructed schedule a fully matched
campaign. Rejected because same seed/configuration does not prove the original
wall-clock trace or exact selection strategy.

## Decision 2: Protocol-derived availability only

**Decision**: The harness controls Provider availability, but clients cannot
read that state. NSC observes timeout/Nack; gRPC observes active probes and RPC
status.

**Rationale**: Passing the simulator oracle to a client would measure an
oracle-assisted scheduler rather than the baseline protocol.

## Decision 3: One common attempt budget

**Decision**: Use 200 ms for both NSC attempts and gRPC probes/RPC attempts,
with a common 5-second logical deadline.

**Rationale**: 200 ms is the frozen NDNSF ACK collection budget. A shared value
avoids system-specific tuning. A short all-in-range smoke may reject the value
for both systems, never tune one system independently.

## Decision 4: Active application health probing

**Decision**: Use a separately identified, separately counted unary health
probe over the installed gRPC stack; do not install a new runtime dependency
solely for the benchmark.

**Rationale**: A paused application can leave TCP/channel state apparently
ready. Explicit probes make recovery policy observable and charge its traffic.

## Decision 5: Descriptive one-shot cells

**Decision**: Run one cell per system/range combination, exactly once.

**Rationale**: This is the requested minimum proposal-slide remediation.
Results are descriptive and carry no variance or significance claim.
