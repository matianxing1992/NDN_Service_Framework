## Material Passport

- **Artifact ID**: `spec144-uav-sensor-stream-generality-plan-v1`
- **Type**: Code experiment plan
- **Mode**: ARS experiment-agent / plan
- **Verification status**: DESIGNED / NOT EXECUTED
- **Created**: 2026-07-24
- **Primary evidence basis**: Current NDNSF/UAV source; frozen Spec 125-128
  design and completion artifacts
- **External literature**: None required for this engineering confirmation
- **Claim boundary**: Two-node MiniNDN, two workloads, frozen profiles only

## Research Questions

- **RQ1**: Can the current generic NDNSF Streaming and adaptive prefetch path
  sustain a 20 Hz, 256-512 byte, single-item, no-FEC UAV telemetry stream with
  bounded AoI, high future-hit utility, low nonproductive Interest cost, and
  reliable progress under the frozen loss/reorder profiles?
- **RQ2**: Can the same generic path sustain 40 ms opaque acoustic/audio blocks
  with variable 2-4 source extents and two-erasure recovery with bounded
  end-to-end latency, complete-block delivery, and explicit protection cost?
- **RQ3**: Are these behaviors achieved without Core/binding decisions selected
  by UAV, telemetry, audio, codec, workload, or cell identity?

## Hypotheses

- **H1**: Telemetry passes zero loss and at least 4/5 repetitions in every
  impaired profile under the preregistered delivery, AoI, gap, future-hit, and
  Interest-utility gates.
- **H2**: Acoustic/audio passes zero loss and at least 4/5 repetitions in every
  impaired profile under the preregistered complete-block, latency, gap,
  future-hit, recovery, and Interest-utility gates.
- **H3**: The neutrality audit finds zero application-selected Core/binding
  branch.

Failure to support H1 or H2 yields a negative workload verdict. Failure of
either blocks the shared generality claim.

## Variables

### Independent variables

- workload family: telemetry or acoustic/audio;
- network profile: zero-loss, loss-only, reorder-only, combined;
- repetition identity for impaired stochastic treatments.

### Dependent variables

- unique sample/source/block delivery and continuity;
- AoI or end-to-end mean/p50/p95/p99/max;
- longest accepted-item gap;
- future-hit and Mapping novelty ratios;
- Mapping/Payload Interest counts and utility categories;
- retry, timeout, Nack, late, skip, exhaustion;
- recovery attempts/success/exhaustion/provenance;
- cell and treatment verdict.

### Controlled variables

- two-node topology and link bandwidth;
- source/binary/config/analyzer identities;
- corrected Spec 145 reference evidence identity and APP/Core ownership
  boundary;
- provider/consumer security and trust configuration;
- 5-second warm-up and 60-second measured window;
- workload cadence, count, payload/extent schedule, FEC rule;
- Mapping/prefetch policy and resource bounds;
- qdisc installation procedure;
- runner timeout and cleanup ownership;
- metric formulas and thresholds.

## Potential Confounds and Controls

| Confound | Control |
|---|---|
| Random application payload shape | deterministic payload and extent cycles |
| Audio hardware/codec scheduling | deterministic/file-backed opaque source |
| Unsynchronized clocks | shared-host monotonic clock; narrow claim |
| Warm-up traffic mixed with latency | explicit full-run traffic vs measured-window latency scopes |
| Repair Data mislabeled useful | three-way Interest utility accounting |
| Selective rerun | one invocation, immutable terminal cell, unique path |
| Concurrent MiniNDN cleanup | one campaign owner and process manifest |
| qdisc attached to wrong interface | discover, verify, and capture both endpoint qdiscs |
| Result-driven tuning | freeze all hashes and thresholds before first formal cell |
| One workload masking another | independent treatment/workload verdicts |
| Reference implementation drift | verify promoted Spec 145 result hash before implementation and at closure |
| Video semantics leaking into new workloads | CodeGraph/source audit plus negative literal/selector checks |

## Experimental Design

This is a controlled engineering experiment with a fixed factorial structure:

```text
2 workloads x (1 zero + 5 loss + 5 reorder + 5 combined) = 32 cells
```

Zero loss is a deterministic acceptance guard, not a statistical sample.
Five repetitions per impaired treatment match the established engineering
campaign convention and support exact accepted-count intervals. The study does
not claim population inference or physical-wireless generalization.

## Analysis Strategy

1. Validate identity, readiness, duration, expected count, qdisc, process, and
   metric-conservation gates before interpreting performance.
2. Compute per-cell application metrics from raw sample/block records and
   network metrics from the unsampled Core terminal-attempt ledger.
3. Apply the frozen cell gates separately to telemetry and acoustic/audio.
4. For each five-repetition treatment, report accepted count and exact 95%
   binomial interval; require at least 4/5.
5. Report raw cell values, not only treatment averages.
6. Attribute failures to explicit gates without discarding them.
7. Run neutrality and historical-hash audits.
8. Reverify the Spec 145 reference identity and prove its video-specific
   semantics did not enter either new workload or any formal denominator.
9. Issue independent workload verdicts and then the shared bounded verdict.

No p-value or causal statement is required. Effect evidence is the absolute
distance from preregistered engineering thresholds plus the full cell
distribution.

## Reproducibility and Stop Rules

- Deterministic tests and smoke runs may be repeated only before formal freeze.
- Formal cells are never automatically retried.
- A formal infrastructure/application failure remains in the denominator.
- Source/config/analyzer drift after freeze stops the campaign and produces an
  incomplete/negative Spec; it does not authorize rebuilding and resuming.
- Raw logs, per-cell summaries, CSV, campaign manifest, commands, hashes, and
  qdisc/process evidence are retained under one fresh result root.

## Planned Outputs

- 32 per-cell JSON summaries;
- one 32-row formal cell CSV;
- one campaign manifest and aggregate JSON;
- exact terminal-attempt counters plus raw sample/block records;
- telemetry and acoustic/audio distribution tables/figures;
- Interest utility and recovery tables;
- neutrality and historical-evidence audit;
- final report with independent and shared verdicts.
