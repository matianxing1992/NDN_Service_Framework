# Preregistered Experiment Plan: Spec 127

## Material Passport

- Artifact: code experiment plan
- Schema: ARS-compatible planning note
- Verification status: PROPOSED
- Version label: `spec127_cross_application_v1`
- Frozen before first live command: required

## Research question and hypothesis

**RQ**: Can the unchanged Mapping v2 adaptive sample-atomic mechanism provide
timely, continuous, Interest-efficient proactive delivery for both a periodic
small-sample stream and a variable-size multisegment opaque stream without
application-specific Core logic?

**Engineering hypothesis**: Both independently satisfy SC-001..SC-009 under
zero loss and the retained Spec 126 combined profile. This is not a population
hypothesis and no significance test is preregistered.

## Variables

- Independent variables: workload family; network profile.
- Dependent variables: complete delivery, order/duplicates/partials, stall,
  exact latency, future-hit ratio, Payload overhead, Mapping novelty, retry,
  timeout, Nack, recovery/skip outcomes.
- Controlled variables: 10 Hz cadence, 5-second warm-up, 60-second measured
  window, topology endpoints, security, logging, interest limits, source
  revision, runner revision, impairment parameters, analyzer revision.
- Known confounds: unseeded kernel netem packet sequence, host scheduling,
  cryptographic timing, process startup. Repetition identity and effective
  qdisc counters are retained; exact packet sequence reproducibility is not
  claimed.

## Sample and ordering

Twelve cells are frozen before execution: two workload/profile zero-loss cells
and ten combined-profile repetitions. Workload sequence seed is `12720260720`;
campaign order seed is `12720260721`. Cell order is deterministically shuffled
before execution. Every cell runs once. Exact intervals accompany 4/5 counts.

## Analysis strategy

- Apply mechanical per-run checks before aggregation.
- Aggregate accepted counts by workload/profile; report exact intervals.
- Report per-run and treatment summaries for every required traffic field.
- Report latency only with exact identity coverage and sample counts; missing
  coverage fails the run.
- Make no causal comparison, p-value, or population reliability claim.
- Preserve failures, invalid evidence, and negative efficiency results.

## Stop conditions

Do not start the matrix until deterministic/security/build gates, single-writer
ownership, free-space preflight, frozen manifests, and source/history hashes
pass. Stop the campaign launcher on source drift, workload drift, missing qdisc
proof, concurrent cleanup ownership, or output reuse. Do not automatically
retry the interrupted cell.
