# Research: Submission-Ready NDNSF Evidence

## Decision 1: Preserve scientific core, not unverifiable historical precision

**Decision**: Preserve the May 20 transaction, security, selection, API, and evaluation concepts. Retain an old numerical result only when its source can be reconstructed from a canonical artifact; otherwise replace it with a current matched experiment or remove the precision.

**Rationale**: The missing physical-node and several mechanism artifacts cannot be recovered from the workspace, while their mechanisms remain implemented. A reproducible narrower paper is stronger than a broader paper whose exact values depend on commit-message recollection.

**Alternatives considered**:

- Keep every old table and cite the historical PDF: rejected because a PDF is the claim, not independent evidence.
- Remove the entire old evaluation: rejected because loss, admission, selection, and physical integration are useful parts of the original scientific story.

## Decision 2: Recover before rerunning

**Decision**: Search retained summaries, structured archives, Git history, and documented commands before launching experiments. The 1/3/10% loss results are retained and will not be rerun. The physical-node table has only a historical commit record and will not be presented as reproducible quantitative evidence.

**Rationale**: Existing canonical results avoid needless computation and preserve the conditions actually reported. Rerunning on a changed runtime would produce a new result, not restore the provenance of an old one.

**Alternatives considered**:

- Rerun every table: rejected as wasteful and methodologically misleading where hardware or code changed.
- Treat directory names as proof: rejected because directory names do not establish inputs or output values.

## Decision 3: Compact replacement campaign

**Decision**: Replace the missing central/supporting comparisons with the smallest registered matrix that tests their intended claims:

1. one Provider at 10 and 100 RPS for NDNSF, NSC, and gRPC;
2. NDNSF admission enabled/disabled at 70 and 100 RPS;
3. FirstResponding versus custom selection at 30 RPS with three heterogeneous Provider delays;
4. the existing Selective-ACK regression as a correctness gate, not a population-level performance estimate.

Each performance cell uses three independent process repetitions and a 60-second measured window. Results report every repetition and variation. Existing central mobility and work-efficiency claims already use larger seed sets and are not expanded.

**Rationale**: Low/high load endpoints establish the baseline trend, and 10/100 RPS have exact 100/10 ms pacing in the current NSC harness. The 70/100 RPS admission pair straddles overload within one NDNSF scheduler, so cross-system millisecond-interval rounding is not involved. One representative heterogeneous-Provider rate tests the selection mechanism. More rates would increase cost without materially changing the claim.

**Alternatives considered**:

- Recreate all four or five historical rates: rejected because these tables are contextual rather than central, and the endpoints plus variation answer the stated questions.
- Use one run per cell: rejected because process and scheduling variation would remain invisible.
- Launch a new range/speed mobility matrix: rejected because the existing registered holdout already supports the bounded mobility claim.

## Decision 4: Matched comparison and failure policy

**Decision**: Share topology, provider delay, workload schedule, measured duration, warm-up, deadlines, repetition seeds, and success definition across systems. A cell is invalid if the measured offered rate is below 80% of its registration or if infrastructure/startup gates fail. Invalidity triggers a matched-block rerun, never selective replacement based on outcome.

**Rationale**: The prior mobility debugging showed that unmatched barriers and traces can create a false advantage. Pre-specified invalidation prevents favorable post-hoc filtering.

**Alternatives considered**:

- Compare whatever each tool produces by default: rejected because defaults encode different timing and load semantics.
- Exclude anomalous repetitions after seeing results: rejected because it invites outcome-dependent selection.

## Decision 5: Artifact authority

**Decision**: Raw runs remain under `results/`, while the Spec evidence directory retains registration, per-run manifests, summarized request outcomes, analysis output, hashes, and a table/figure index. No manuscript claim may point only to a temporary path.

**Rationale**: Project policy treats `results/` as local output, not durable truth. Small canonical summaries are sufficient to audit claims without retaining unlimited logs.

**Alternatives considered**:

- Commit every raw log: rejected because it creates large, noisy, duplicated evidence.
- Retain only manuscript tables: rejected because transformation and exclusions would be opaque.

## Decision 6: Statistical reporting

**Decision**: For three-repetition supporting cells, report all repetition values and mean plus standard deviation (or median and range when distributions are visibly skewed). Do not use packet-level observations as independent replicates. Existing seed-level mobility inference remains seed-level.

**Rationale**: Requests within one process share network and runtime state. Treating them as independent would underestimate uncertainty. Three repetitions are sufficient to expose gross instability for supporting tables but not to support subtle population claims.

**Alternatives considered**:

- Bootstrap individual requests: rejected as pseudoreplication.
- Claim statistical significance from three runs: rejected as underpowered and unnecessary for contextual mechanisms.
