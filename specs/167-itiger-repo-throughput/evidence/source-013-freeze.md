# Spec 167 source-013 freeze and formal result

Status: `REMOTE_STAGED_AND_VERIFIED; PREFLIGHT_PASS; FORMAL_CAMPAIGN_PASS` (2026-08-02)

Source-013 is the single authorized replacement for the immutable source-012
packaging failure. It reuses the existing SIF, schedule, and Repo payload; no
model, foundation image, compiled extension, or SIF was prepared or rebuilt.
Source-012 and job 181820 remain unchanged failure evidence.

## Immutable identity

| Field | Value |
|---|---|
| Remote source | `/project/tma1/ndnsf-di/jobs/spec167/source-013` |
| Source manifest SHA-256 | `0ad0372c0f4c36e38cb24e3edc570ebaada8aba517c4d937acd68c0e33b5d4ee` |
| File checksum-list SHA-256 | `7521f8430db61bd95346d36c9228be8817c897726bfd92e4158e47094e7cd280` |
| Campaign identity | `spec167-tiger-20260802-source013` |
| Formal Slurm job | `181822` |
| Nodes | `itiger07,itiger08` |
| Hard deadline | `03:00:00` |
| Final evidence | `/project/tma1/ndnsf-di/evidence/spec167/campaign/spec167-tiger-20260802-source013` |

The remote source checksum ledger passed. The corrected 16-file bundle includes
the existing runtime dependency `Experiments/spec164_artifact_campaign.py`,
which was absent from source-012 and caused its pre-transport import failure.

## Preflight and formal acceptance

The authorized two-node repository preflight was submitted once as
`spec167-repo-preflight-013` (job `181821`) and passed. Its durable evidence is
`/project/tma1/ndnsf-di/evidence/spec167/preflight/spec167-repo-preflight-013`;
both warm and cold 64 MiB transfers reported 67,108,864 logical bytes.

The formal job `181822` completed with exit code 0 in `02:34:53`. The terminal
record is `PASS` and the analyzer emitted `SPEC167_ANALYSIS_PASS`:

| Acceptance field | Value |
|---|---:|
| Expected rows | 60 |
| Observed rows | 60 |
| Warmups | 10 |
| Measured rows | 50 |
| Measured failures | 0 |
| Missing / duplicate / unexpected rows | 0 / 0 / 0 |
| Path violations | 0 |

All 60 run records have status `PASS`. The remote `checksums.sha256` ledger was
verified before promotion, and the complete sanitized evidence was copied to
`results/spec167-source-013-freeze/remote-evidence/`, where the same ledger
also passes.

## Measured summary

Median logical goodput in the analyzer output (Mbps; five measured repetitions
per subject) was:

| Payload | Physical | Raw segmented NDN | Digest-only | Signed manifest | Legacy exact packet |
|---:|---:|---:|---:|---:|---:|
| 64 MiB | 940.852 | 332.191 | 325.870 | 323.942 | 52.141 |
| 1 GiB | 940.853 | 336.125 | 327.271 | 328.983 | 52.334 |

These are matched Spec 167 subject results, not a claim that every deployment
will reach line rate. The evidence preserves the per-run records, bootstrap
intervals, cold-transfer measurements, and analyzer bootstrap confidence
intervals for later reporting.

## Source-012 closure boundary

The source-012 `ModuleNotFoundError` was reproduced locally and fixed only by
including the already-existing helper in source-013's manifest. No transport
or DistributedRepo throughput conclusion is drawn from the source-012 failure.
The source-012 remote tree, job `181820`, and `.partial` evidence remain
immutable and are referenced by `evidence/source-012-freeze.md`.

