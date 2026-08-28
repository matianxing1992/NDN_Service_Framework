# Spec 167 source-011 freeze

Status: `LOCAL_FROZEN_PENDING_REMOTE_STAGE` (2026-08-02 UTC)

This is the only candidate source identity for the next formal campaign. It is
restricted to the current Spec 167 runner, the already-implemented bounded
peer-face retry, the `srun < /dev/null` stdin isolation fix, and the local
contract tests that enforce those two invariants. It contains no model,
foundation image, compiled extension, or generated payload copy.

## Identity

| Field | Value |
|---|---|
| Source ID | `source-011` |
| Previous source | `/project/tma1/ndnsf-di/jobs/spec167/source-010` |
| Local staging | `results/spec167-source-011-freeze/` |
| Source-checksums SHA-256 | `905078b2809453c38de00bcc971ba2d0a84105825026535bdce6c1dc8514d22f` |
| Source manifest SHA-256 | `f830592fc316a88f8942b9cb3fe49e05ca1fe5bf9ef5568c9a3ca380f3c576b1` |
| Files | 15 source/test files, 160,184 bytes |

The authoritative file-level manifest and relative checksum file are kept in
the staging directory above. `sha256sum -c source-checksums.sha256` passes
from that directory.

## Allowed delta from source-010

- `campaign-rank-inner.sh`: bounded peer-face creation (up to 300 attempts,
  two seconds per attempt, with a 30-second overall budget).
- `repo-throughput.sbatch`: every formal `srun` receives stdin from
  `/dev/null`, so it cannot consume the frozen `schedule.tsv` loop input.
- `test_spec167_itiger_job_contract.py`: assertions for both invariants.

All other included files are the existing Spec 167 runner/runtime boundary and
its artifact/analyzer contracts needed to execute the same campaign.

## Reused inputs

- SIF: `/project/tma1/ndnsf-di/releases/spec166-dcef2858c060/runtime.sif`
- SIF materialization: the existing `materialization.json` beside that SIF;
  no SIF regeneration or foundation rebuild.
- Deterministic schedule: seed `16720260731`, exactly 60 rows (one warmup plus
  five measured repetitions for each preregistered cell).
- Existing Spec 167 controlled Repo payload fixture semantics; no Qwen/model
  download or preparation is part of this campaign.

The remote copy must be staged under a new immutable
`/project/tma1/ndnsf-di/jobs/spec167/source-011` directory and verified before
the single formal submission. Until that remote checksum and existing-input
check pass, no `sbatch` submission is authorized.
