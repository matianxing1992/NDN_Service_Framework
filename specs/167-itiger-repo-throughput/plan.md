# Implementation Plan: TigerCluster DistributedRepo Throughput Validation

**Branch**: `167-itiger-repo-throughput` | **Date**: 2026-07-31 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/167-itiger-repo-throughput/spec.md`

## Summary

Build a deployment-faithful, two-node TigerCluster benchmark around the already
implemented Spec 164 artifact subjects.  Separate rank-local payload/storage
from the small shared coordination ledger, run every data byte over the same
cross-node NFD route, freeze the candidate/source/schedule before exact-once
submission, and independently analyze physical, raw NDN, legacy, digest-only,
signed-manifest, cold-retrieval, and warm-reuse evidence.

Spec 162's 2026-08-01 Qwen3.6 requalification is an input to the phase
boundary, not a benchmark subject: its 53.79 GB cold model path exposed a
separate interval for root-manifest/catalog activation after payload staging,
and its total Slurm time also includes Provider preparation and DI control
traffic. The formal Spec 167 campaign must measure those repository phases with
controlled payloads and matched network subjects rather than reuse Qwen job
elapsed time.

The Qwen3.6-27B smoke also exposed a transport-fairness defect in the
artifact client: every large fetch could grow to an 8/128 Interest window with
a 32-packet persistence backlog, a 10-second per-Interest lifetime, and five
retries. Three simultaneous stage pulls could therefore starve one shard until
its retry budget expired. The implementation now uses a bounded 4/16/16
window/backlog profile, a 30-second Interest lifetime, and eight retries, with
clamped environment overrides. This is a repair to validate before the formal
campaign; the 181951 failure remains immutable negative evidence and is not
rewritten as a throughput result.

## Technical Context

**Language/Version**: Python 3.10+, Bash, existing C++ NDNSF/ndn-cxx runtime  
**Primary Dependencies**: accepted Spec 166 SIF, NFD/nfdc, python-ndn, NDNSF-DistributedRepo artifact-manifest-v2, Slurm, Apptainer, checksum-bound Python TCP ceiling  
**Storage**: rank-local `$SLURM_TMPDIR` for measured payload/store state; `/project/tma1/ndnsf-di` only for immutable inputs, coordination markers, and promoted evidence  
**Testing**: Python `unittest`, shell contract tests, exact candidate-container import probe, local loopback NFD smoke, immutable remote preflight  
**Target Platform**: TigerCluster `bigTiger`, account `devs`, two fixed compute nodes, CPU-only  
**Project Type**: experiment runner plus immutable Slurm job/evidence analyzer  
**Performance Goals**: measure absolute goodput and paired ratios without claiming link saturation; at least 60 seconds per rate observation  
**Constraints**: one warmup plus five measured repetitions per cell; no replacement of measured failures (the single source-013 packaging-closure correction is a separately documented exception); no payload through shared NFS; no GPU allocation; no private material in evidence  
**Scale/Scope**: 64 MiB and 1 GiB; r1/c1; five subjects; 10 warmups and 50 measured repetitions

## Constitution Check

| Principle | Gate | Status |
|---|---|---|
| Canonical Dynamic Runtime | Reuse generic artifact/NDNSF paths; no service-specific framework messages | PASS |
| Security Is Part Of The Data Path | Signed root, digest chain, bounded parsing, exact cleanup, no public shared HMAC default | PASS |
| CodeGraph First | Current Spec 164 roles, stores, callers, and Spec 166 job pattern inspected before design | PASS |
| Spec-Driven Durable Work | Spec 167 owns plan, contracts, tasks, source identity, negative evidence, and result | PASS |
| Verify With The Right Scope | Unit and exact-container gates precede one immutable TigerCluster submission | PASS |
| Cohesive Outcome-Based Tasks | Tasks group tests, behavior, focused validation, and evidence by acceptance outcome | PASS |

## Architecture

```text
allocation: itiger07 + itiger08 (CPU-only)

rank 0 local scratch                     rank 1 local scratch
publisher payload/store                  repository payload/store
raw producer / cold consumer             raw consumer / repo / cold producer
          |                                        |
          +--------- NFD TCP face ----------------+

/project/tma1/ndnsf-di/evidence/spec167
  coordination markers + append-only ledgers + final sanitized evidence only
```

The reusable Spec 164 role logic gains an explicit separation between
`coord_dir` and `data_dir`.  Coordination JSON and readiness markers may use
shared project storage; payloads, repository databases, and cold destinations
must resolve under node-local scratch.  Publication travels rank 0 to rank 1;
cold retrieval travels rank 1 to rank 0.  The raw subject uses the same NFD
face and packet geometry.  Physical ceiling uses the same nodes/interface.

## Gate Order

1. Validate Spec Kit artifacts and source traceability.
2. Run unit/contract tests for directory separation, schedule, analyzer,
   executable modes, package closure, evidence schema, cleanup, and log
   atomicity.
3. Probe the exact local candidate image with no source-only dependency.
4. Stage an immutable source bundle and checksum manifest under a new Spec 167
   identity.
5. Run a bounded CPU Slurm preflight that proves SIF imports, NFD/nfdc,
   TCP streaming, writable rank-local scratch, node/interface identity, and source/SIF
   checksums.  It makes no throughput claim.
6. Submit the formal campaign exactly once under a new immutable identity.
7. Monitor progress markers every 30--60 seconds with a hard deadline.
8. Promote PASS or retain `.partial` failure evidence without rerunning or
   changing the frozen subject.
9. Independently recompute checksums, counts, paired ratios, bootstrap
   intervals, and claim boundaries locally.

## Project Structure

### Documentation (this feature)

```text
specs/167-itiger-repo-throughput/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── tasks.md
├── audit.md
├── contracts/
│   ├── experiment-contract.md
│   └── evidence-schema.md
├── checklists/requirements.md
├── jobs/
│   ├── repo-throughput-preflight.sbatch
│   ├── repo-throughput.sbatch
│   └── rank.sh
└── evidence/
```

### Source Code (repository root)

```text
Experiments/
├── NDNSF_DistributedRepo_Artifact_Minindn.py
├── NDNSF_DistributedRepo_Artifact_Itiger.py
└── analyze_spec167_itiger_repo.py

tests/python/
├── test_spec167_itiger_artifact_runner.py
└── test_spec167_itiger_job_contract.py
```

**Structure Decision**: Keep protocol/runtime implementation in
`NDNSF-DistributedRepo`; Spec 167 adds only portable experiment-role boundaries,
Slurm orchestration, and analysis.  No distributed-inference logic moves into
base NDNSF.

## Statistical Plan

- Freeze randomized within-repetition subject order before submission.
- Retain one warmup and five measured repetitions for every subject/size cell.
- Report every sample, completion denominator, median, IQR, p50, p95, and
  bootstrap 95% confidence interval.
- Primary paired contrasts: signed/legacy, digest/raw, signed/digest, and
  subject/physical ceiling.
- Treat failed measured transfers as retained zero-goodput outcomes for ratio
  acceptance where the pair exists; otherwise classify the pair incomplete and
  fail campaign acceptance.
- Do not pool sizes, cache states, or unmatched observations.
- Check the ARS 11-fallacy set before any performance claim.

## Risk and Rollback

- **Candidate drift**: block if the accepted SIF plus checksum-bound overlay
  cannot import current artifact runtime; create a new candidate identity rather
  than modifying the accepted Spec 166 release.
- **NFS contamination**: resolve and record every payload/store path; analyzer
  rejects any path under `/project` or `/home`.
- **Queue or node change**: exact nodelist is frozen; a different allocation is
  a new campaign, not an automatic retry.
- **Long runtime**: durable progress prevents false silence timeout; a bounded
  hard deadline remains authoritative.
- **Failure**: preserve `.partial`, terminal record, logs, source checksums, and
  Slurm state; do not promote or overwrite.
- **Rollback**: remove only a newly staged, checksum-verified unused Spec 167
  source directory; never remove accepted Spec 164/166 artifacts or evidence.

## Complexity Tracking

| Complexity | Why Needed | Simpler Alternative Rejected |
|---|---|---|
| Separate coordination and rank-local data directories | Prevent shared NFS from becoming the measured transport/store path | Reusing one shared run directory would invalidate cross-node throughput |
| Two-stage preflight then formal submission | Exact SIF capabilities cannot be inferred from local Docker or login-node tools | Direct formal submission previously exposed locally detectable defects |
| Paired randomized schedule | Controls cluster drift and supports defensible ratios | Sequential subject blocks confound subject with time |
