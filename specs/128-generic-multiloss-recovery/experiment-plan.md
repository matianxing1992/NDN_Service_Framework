# Preregistered Experiment Plan: Spec 128

## Material Passport

- Artifact: code experiment plan
- Schema: ARS-compatible planning note
- Verification status: EXECUTED / NEGATIVE
- Version label: `spec128_generic_multiloss_recovery_v2`
- Frozen before first live command: required

## Research question and engineering hypothesis

**RQ**: Can a generic bounded exact-name future-Interest retry plus a declared
multi-loss recovery contract restore robust opaque-stream delivery under the
Spec 127 failure boundary, without introducing application or payload
specialization?

**Engineering hypothesis**: Both the periodic one-item/no-FEC family and the
variable multi-segment family independently meet Spec 128's correctness,
bounded-work, future-hit, and continuity criteria. The capacity-plus-one
profile must fail closed. This is an engineering acceptance test, not a
population hypothesis; no p-value or causal claim is preregistered.

The immutable `confirmation01` and `confirmation02` campaigns are negative
design evidence, not treatment data for v2. Before any v2 cell, confirmation02
showed that variable-extent zero-loss traffic has a 95.46% raw future-hit ratio
because authenticated extent advice terminalizes bounded predictor
overprediction, while exact periodic traffic has no such structural work.
Accordingly v2 preregisters a 99% zero-loss hit gate for the exact periodic
stream and a 95% raw hit gate for the variable-extent stream; both ratios and
terminal advice remain separately reported. It also binds speculative Mapping
Interest lifetime to the full declared Mapping-ahead horizon while retaining
RTT-driven recovery for the current Mapping gap.

## Variables and confounds

- Independent variables: workload family; zero-loss, multiple-loss/retry, and
  capacity-plus-one impairment profile; declared generic recovery capacity.
- Dependent variables: ordered complete delivery, byte identity, terminal
  reason, recovery count, retries, future hits, utility/overhead, Mapping
  novelty, timeout, Nack, latency, stall, and coverage.
- Controlled variables: topology, security, source/binding revision, cadence,
  warm-up and measured window, workload/order seed, logging, qdisc settings,
  runner/analyzer revision, and resource caps.
- Known confounds: unseeded kernel netem packet realization, host scheduling,
  cryptographic timing, content-store satisfaction, and process startup. Keep
  qdisc proof, timestamps, repetition identity, and satisfaction provenance;
  do not claim exact packet-level replication.

## Design and analysis

The implementation plan must freeze a complete 16-cell matrix before execution:
one zero-loss guard per workload; five impaired acceptance repetitions per
workload; and two capacity-plus-one safety cells per workload. The five-repeat
groups are the only reliability aggregates and must use exact intervals. The
two safety cells are deterministic boundary checks and must all pass their
fail-closed gate.

All cells use distinct output directories below a newly created
`results/spec128-*` root. The runner validates a single cleanup/launcher owner,
free space, manifest hashes, no destination reuse, and unchanged Spec 127
baseline hashes before launch. A cell executes once. If interrupted, invalid,
or failed, preserve it in the aggregate and do not automatically rerun it.

Analyze every cell mechanically before aggregation. Attribute initial and retry
Payload Interests separately; label provider-confirmed future hits separately
from cached/retained Data satisfaction; report recovery capacity and actual
missing-source count. Report exact coverage and all unavailable fields. A
positive result requires both workload families to pass independently. A
threshold miss or safety failure is a measured negative result, not authority
to tune and rerun a formal cell.

## Stop conditions

Do not launch any live cell until deterministic, compatibility/security,
build/binding, neutrality, strict Spec Kit, campaign-runner, baseline-hash,
single-writer, qdisc-proof, free-space, and frozen-manifest gates pass. Stop
the launcher on source/configuration/baseline drift, concurrent cleanup owner,
missing impairment proof, or output reuse. Preserve the current cell and
report the stop; do not start a replacement cell automatically.
