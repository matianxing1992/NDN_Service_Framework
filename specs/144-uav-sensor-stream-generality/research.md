# Research Decisions: UAV Sensor Stream Generality

## Decision 1: Validate existing UAV telemetry before adding microphone hardware

**Decision**: Make compact 20 Hz telemetry the first workload and retain
`GetStatus` for complete snapshot/fallback behavior.

**Rationale**: UAV-APP already produces position, speed, battery, readiness,
and link state, while Ground Station currently polls status every 1.5 seconds.
This creates a real application use case and directly exercises the periodic
one-item/no-FEC boundary left negative by Spec 128 without capture-device or
codec confounds.

**Alternatives considered**:

- Start with live microphone capture: rejected because ALSA/device scheduling,
  audio fidelity, and codec latency would obscure transport causality.
- Replace `GetStatus`: rejected because request/response remains appropriate
  for complete snapshots, startup, and fallback.

## Decision 2: Use fixed schedules rather than random application payload shape

**Decision**: Telemetry cycles 256/384/512 bytes at 20 Hz. Acoustic/audio uses
40 ms blocks whose source counts cycle 2/3/4 and whose source bytes are
deterministic.

**Rationale**: Fixed schedules make expected counts and byte identity
reproducible. Network faults remain stochastic treatments; application shape
must not become a second uncontrolled random variable.

**Alternatives considered**:

- Random payload size/source count: rejected because it weakens cross-run
  comparison and complicates exact reconstruction checks.
- One constant payload size: rejected because it would not exercise the
  requested 256-512 byte range.

## Decision 3: Treat audio as opaque continuous-media blocks

**Decision**: Validate short opaque blocks and generic two-repair recovery, not
a named PCM/Opus format or actual playback.

**Rationale**: The generality question is whether Core handles another cadence
and extent family. Codec/device behavior belongs to the application and would
violate the neutrality objective if embedded in Core.

**Alternatives considered**:

- One 20 ms source item with two repairs: rejected because protection overhead
  is extreme and it repeats the single-item shape.
- Codec-specific recovery: rejected because it cannot support the genericity
  claim.

## Decision 4: Keep three Interest-utility categories

**Decision**: Report application-useful, protection-only, and nonproductive
Interest outcomes separately.

**Rationale**: A repair Data packet may arrive successfully but never be
consumed in zero loss. Calling it application-useful hides protection cost;
calling it a failed Interest is also incorrect. The three-way attribution
supports honest assessment of both efficiency and reliability.

**Alternatives considered**:

- Count every returned Data as useful: rejected because it hides unused repair
  cost.
- Count every unused repair as a network failure: rejected because it is an
  intentional redundancy decision.

## Decision 5: Reuse fault semantics, not historical executions

**Decision**: Use the four Spec 126 profile definitions in a wholly new
32-cell matrix while never invoking or modifying Spec 127/128 runners/results.

**Rationale**: The same impairment semantics support comparison with earlier
boundaries. Fresh commands, output paths, workloads, hashes, and invocations
prevent historical evidence contamination.

**Alternatives considered**:

- Append new cells to Spec 127/128: rejected by the user and by frozen-evidence
  policy.
- Test only zero loss: rejected because it cannot address the unresolved
  reliability boundary.

## Decision 6: Separate workload verdicts and require both for generality

**Decision**: Each workload must independently pass zero-loss and treatment-
level gates. A shared positive statement requires both.

**Rationale**: A strong result for two-repair audio cannot compensate for a
failed periodic telemetry boundary, and vice versa.

**Alternatives considered**:

- Pool all 32 cells: rejected because it permits one workload to mask another.
- Report only averages: rejected because tail gaps and failed repetitions are
  controlling outcomes.

## Decision 7: Use one monotonic shared-host clock only within MiniNDN

**Decision**: Capture origin and consumer terminal events use the shared host's
monotonic clock; every metric declares this clock domain.

**Rationale**: It supports reliable one-way AoI/end-to-end measurement in this
test environment without pretending to solve physical-device clock
synchronization.

**Alternatives considered**:

- Wall-clock timestamps: rejected because clock adjustment can corrupt
  durations.
- Claim physical-device one-way latency: rejected as unsupported.

## Decision 8: Freeze before formal execution and stop on formal failure

**Decision**: Deterministic defects may be corrected only before the formal
matrix and followed by renewed audit. Once the first formal cell starts, no
source, binary, configuration, threshold, or analyzer semantics change.

**Rationale**: This prevents result-driven tuning and preserves negative
evidence.

**Alternatives considered**:

- Automatically rerun crash/failure cells: rejected because it changes the
  denominator and can hide real lifecycle instability.
- Fix and resume midway: rejected; a genuine post-freeze defect belongs to a
  new Spec/campaign.

## Decision 9: Use corrected Spec 145 UAV Video as an integration reference

**Decision**: Treat the completed Spec 145 UAV Video path as the formal
APP-side reference for Streaming lifecycle ownership, exact-name/security
admission, callback failure containment, and display of generation-fenced
actual Core state.

**Rationale**: Spec 145 corrected the known non-30-fps, callback-boundary, and
status-truth defects and then passed one fresh 20-fps, 60-second, zero-loss
two-node MiniNDN cell. Reusing that demonstrated ownership pattern avoids
inventing another APP/Core integration while preserving the purpose of Spec
144: testing genuinely different periodic single-item and variable
multisegment workloads.

**Reference evidence**:

- `specs/145-uav-video-runtime-corrections/completion-summary.md`;
- `specs/145-uav-video-runtime-corrections/evidence/post-implementation-audit.md`;
- `results/spec145-uav-video-runtime-20260724T064253Z`;
- campaign summary SHA-256:
  `8bad7f90d12647f2904e208000e189ca73a5369ebb33f359a8f07a5560ca2566`.

**Alternatives considered**:

- Recreate telemetry/audio integration independently: rejected because it
  risks repeating already corrected lifecycle and status-ownership defects.
- Copy UAV Video class/FPS/GOP/codec behavior: rejected because those are
  workload semantics, would invalidate the neutrality claim, and are
  explicitly outside the reference contract.
- Count the Spec 145 cell as Spec 144 evidence: rejected because it is video,
  not telemetry or acoustic/audio, and is not one of the 32 frozen cells.
