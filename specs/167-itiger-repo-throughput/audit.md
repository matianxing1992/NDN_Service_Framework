# Spec 167 Pre-Implementation Audit

**Mode**: implementation-readiness  
**Date**: 2026-07-31  
**Verdict**: **PASS TO IMPLEMENT; REMOTE SUBMISSION BLOCKED UNTIL T001--T004 PASS**

## Gate Results

| Gate | Result | Evidence boundary |
|---|---|---|
| Context Mode | PASS | Health resolves Spec 167 with trusted hooks; repository files remain authoritative |
| CodeGraph | PASS | Current Spec 164 producer/consumer roles, artifact manifest/store code, and Spec 166 Slurm pattern inspected |
| Spec Kit | PASS | Spec, plan, research, data model, contracts, quickstart, checklist, and cohesive tasks agree |
| GSD | PASS | Installation healthy; the experiment has durable phases and immutable handoff artifacts |
| ARS | PASS FOR PLAN | Matched design, negative-evidence retention, paired analysis, and 11-fallacy guard are explicit |

## Necessity and Ownership

- A separate external-validity experiment is necessary because job 181096 used
  pre-staged model artifacts and cannot measure DistributedRepo transfer.
- Protocol and storage behavior remain owned by `NDNSF-DistributedRepo`.
  Spec 167 owns only portable experiment roles, Slurm orchestration, and
  analysis; no DI-specific behavior is added to base NDNSF.
- Reusing the MiniNDN process launcher unchanged is rejected because it requires
  Mininet privileges and places coordination and data beneath one run directory.

## Security and Persistence

- Signed-root and digest-chain trust remain the scalable public trust model;
  HMAC is not introduced as a shared public verification secret.
- Measured stores are rank-local and disposable.  Only manifests, ledgers,
  sanitized logs, and checksums are promoted.
- `.partial` failure evidence is append-only and cannot be overwritten by a
  new attempt.
- Exact candidate and source checksums, secret scans, and cleanup ordering are
  blocking preflight gates.

## Evidence Readiness

No Spec 167 TigerCluster throughput claim exists yet. Spec 164 MiniNDN results
remain the baseline hypothesis. Spec 162's authorized Qwen requalification
jobs 181527, 181528, 181530, 181531, and failed 181532 demonstrate that a
large model can reach the Repo publication path, and expose a measurable gap
between payload staging and registration/catalog `ACTIVE`; they do not provide
matched transport subjects or an isolated goodput result. T001--T004 must
establish portable execution and exact-artifact deployment fidelity before T005
is authorized.

## Findings

1. **MEDIUM — candidate capability is unverified**: the accepted Spec 166 SIF
   has not yet proven current DistributedRepo imports, `iperf3`, or the portable
   Spec 167 role. T003/T004 block formal submission until verified.
2. **MEDIUM — role directory split is not implemented**: current Spec 164 roles
   accept one `run_dir`; T001 must separate coordination from node-local data or
   the result would be NFS-confounded.
3. **LOW — two fixed nodes limit generalization**: the first campaign isolates
   transport overhead. Node-pair replication belongs to a follow-up study.

No CRITICAL or invalidating HIGH issue remains in the plan.  Implementation may
start, but no formal Slurm campaign may be submitted before the stated gates.

## Failure Reaudit Addendum — 2026-08-01

**Current verdict: CONDITIONAL PASS FOR THE RUNNER; BLOCK FORMAL THROUGHPUT
CLAIMS UNTIL T005 COMPLETES.** T001--T004 are now complete. Job `181106` passed
the two-node 64 MiB preflight at 335.06 Mbps forward and 312.18 Mbps reverse;
job `181110` passed the formal-path smoke after cross-node control barriers
moved off `/project` NFS. These are deployment/preflight observations, not a
formal distribution.

The formal campaign remains incomplete. Job `181112` preserved the TCP-face
listener race; job `181115` then completed one signed-manifest warmup but
stopped because `srun` consumed the remaining `schedule.tsv` rows from stdin.
Current source uses bounded face retries and redirects every `srun` stdin from
`/dev/null`; the source/job-contract gate owns both regressions. The failed job
identities and their rows remain immutable and will not be retried or relabeled.

The complete cross-Spec failure classification and no-repeat asset plan is
recorded in
`specs/165-real-di-validation-gates/evidence/failure-and-retention-audit-20260801.md`.
The Qwen stage payload now has one content-addressed bundle; no future Spec 165
or DI requalification run may copy the 5--6 GB tree into a new result directory.

For this addendum, Context Mode health failed closed because the project had no
bound ContentDB (`found 0`); repository documents, CodeGraph, immutable job
records, and result manifests were used as authority. This does not invalidate
the older pre-implementation Context Mode check, but it prevents claiming that
Context Mode is healthy now.

## Preserved source-012 formal failure — job 181820

The one authorized replacement campaign used immutable source-012:

- Campaign: `spec167-tiger-20260802-source012`
- Source manifest SHA-256: `6329acb5e0f4d2e61a44e8eed810abe6e0ff4ad32812719fa718d68b92bf8f79`
- SIF SHA-256: `e82d5d4b9cedacb2cb60451d7ecdf624732953359bd680fe235ba8db44c2f45a`
- Slurm job: `181820`, `FAILED 1:0`, `00:42:51`, hard limit `03:00:00`
- Evidence: `/project/tma1/ndnsf-di/evidence/spec167/campaign/.spec167-tiger-20260802-source012.partial`
- Ledger: 60/60 rows retained; 12 physical-network passes and 48 repository failures

The repository failures all report `ModuleNotFoundError: No module named
'spec164_artifact_campaign'`. This is a packaging-closure defect: the existing
runtime dependency was present in source-010/source-011 but was omitted from
source-012 while restricting the bundle to the requested fix files. The
437-file failure checksum ledger and sanitized-evidence gate passed. The source
directory and failed campaign remain immutable; no retry or second submission
is part of Spec 167's current freeze.

## Local packaging closure after source-012 failure

The failure was reproduced with the exact candidate container and mounted
source directory: `_benchmark_consumer_role` imports
`spec164_artifact_campaign.self_resource_totals`, but source-012 did not bind
that existing helper. The failure therefore occurs before repository
transport and is not evidence of a DistributedRepo throughput regression.

The local gate now accepts `SPEC167_SOURCE_ROOT` and validates the exact
source-root mount rather than implicitly testing the working tree. A new
unsubmitted candidate at
`results/spec167-source-012-corrected-candidate/` adds only
`Experiments/spec164_artifact_campaign.py`. The source-bundle validator
passes all 16 file hashes and the required runtime closure; the exact image
passes the runner/contract tests, two-container NFD preflight, and all four
repository smoke subjects. Remote source-012 and job 181820 were not
modified, and no second formal submission was made.

## Source-013 formal closure — job 181822

Source-013 is the one authorized replacement for the immutable source-012
packaging failure. Its source manifest SHA-256 is
`0ad0372c0f4c36e38cb24e3edc570ebaada8aba517c4d937acd68c0e33b5d4ee`; its
16-file checksum-list SHA-256 is
`7521f8430db61bd95346d36c9228be8817c897726bfd92e4158e47094e7cd280`. The
reused Spec 166 SIF remains byte-identical
(`e82d5d4b9cedacb2cb60451d7ecdf624732953359bd680fe235ba8db44c2f45a`), and no
model, foundation, or SIF preparation was performed.

The two-node preflight `spec167-repo-preflight-013` (job `181821`) passed. The
formal campaign `spec167-tiger-20260802-source013` (job `181822`, nodes
`itiger07,itiger08`, hard deadline `03:00:00`) completed in `02:34:53` with
exit code 0. The final evidence is
`/project/tma1/ndnsf-di/evidence/spec167/campaign/spec167-tiger-20260802-source013`;
its terminal state is `PASS` and `analysis.log` contains
`SPEC167_ANALYSIS_PASS`.

The acceptance ledger contains exactly 60 rows: 10 warmups and 50 measured
rows. All 60 run records are `PASS`; measured failures, missing rows,
duplicates, unexpected rows, and path violations are all zero. The complete
sanitized evidence was copied to
`results/spec167-source-013-freeze/remote-evidence/` and its checksum ledger
passes locally. The corrected bundle includes the existing
`Experiments/spec164_artifact_campaign.py` runtime dependency, closing the
source-012 import defect without changing the experiment protocol.

The analyzer's median logical goodput was 940.852/940.853 Mbps for the
64 MiB/1 GiB physical baselines, 332.191/336.125 Mbps for raw segmented NDN,
325.870/327.271 Mbps for digest-only, 323.942/328.983 Mbps for signed
manifest, and 52.141/52.334 Mbps for legacy exact-packet subjects. These are
matched campaign observations; per-run records and confidence intervals remain
the authority for any later paper claim.

**Updated verdict: PASS FOR THE SPEC 167 CAMPAIGN.** The source-012 failure is
retained as packaging evidence, while source-013 establishes the first complete
TigerCluster throughput result under the frozen schedule. Further work should
analyze and report the matched subject deltas; it must not silently relabel or
rerun this campaign. The throughput/count/integrity acceptance is closed, but
the full FR-008 resource-cost surface (host CPU, peak memory, wire-byte
breakdown, and complete amplification decomposition) is not emitted by the
current result schema and remains a follow-up instrumentation item.
