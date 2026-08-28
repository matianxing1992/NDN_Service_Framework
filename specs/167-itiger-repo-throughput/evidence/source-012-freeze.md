# Spec 167 source-012 freeze

Status: `REMOTE_STAGED_AND_VERIFIED; FORMAL_CAMPAIGN_FAILED` (2026-08-02)

`source-011` already existed on TigerCluster and has an incomplete
`source011-r2` campaign partial, so it was not reused or overwritten. The new
identity is `source-012`.

## Immutable identity

| Field | Value |
|---|---|
| Remote source | `/project/tma1/ndnsf-di/jobs/spec167/source-012` |
| Source manifest SHA-256 | `6329acb5e0f4d2e61a44e8eed810abe6e0ff4ad32812719fa718d68b92bf8f79` |
| File checksum-list SHA-256 | `905078b2809453c38de00bcc971ba2d0a84105825026535bdce6c1dc8514d22f` |
| Campaign identity | `spec167-tiger-20260802-source012` |
| Failure evidence path | `/project/tma1/ndnsf-di/evidence/spec167/campaign/.spec167-tiger-20260802-source012.partial` |

The remote `sha256sum -c source-checksums.sha256` passed for all 15 files.
The bundle contains only the existing Spec 167 runner/jobs/analyzer and the
two local contract-test sources. The allowed code delta is limited to the
bounded peer-face retry and formal `srun < /dev/null` stdin isolation; no
model, foundation image, compiled extension, or model payload was added.

## Reused inputs

- Existing SIF: `/project/tma1/ndnsf-di/releases/spec166-dcef2858c060/runtime.sif`
- SIF SHA-256: `e82d5d4b9cedacb2cb60451d7ecdf624732953359bd680fe235ba8db44c2f45a`
- Existing SIF materialization record: `materialization.json` beside the SIF;
  its recorded digest matches the SIF.
- Schedule: seed `16720260731`, 60 rows. The new staged manifest produces the
  same `schedule.tsv` as the previous incomplete source011-r2 partial
  (`SCHEDULE_MATCH=True`).
- Repo payload: the existing Spec 167 controlled payload path and fixture
  semantics are retained; rank-local scratch remains the measured data path.
  No model or foundation preparation is performed.

The previous source011-r2 partial reached 59 of 60 rows before its original
02:30 Slurm limit. The new submission must remain a single bounded campaign;
the submission receipt records any explicitly selected longer hard deadline so
that this known scheduling limit does not silently truncate the final row.

## Formal campaign result

Job `181820` was submitted exactly once with this source and terminated
`FAILED 1:0` after `00:42:51`. All 60 scheduled rows were retained: 12
physical-network rows passed and 48 repository rows failed with the durable traceback
`ModuleNotFoundError: No module named 'spec164_artifact_campaign'` from
`NDNSF_DistributedRepo_Artifact_Minindn.py:845`. The module is an existing
runtime dependency present in source-010/source-011 but was accidentally
omitted from this source-012 bundle. The source directory and running job are
immutable and are not repaired in place; the failure remains part of the
campaign evidence, and no replacement submission is authorized by this
freeze.

Terminal evidence:

`/project/tma1/ndnsf-di/evidence/spec167/campaign/.spec167-tiger-20260802-source012.partial`

The 437-file checksum ledger passed, the reused SIF checksum matched its
materialization record, and no private keys, bootstrap tokens, payload copies,
or repository databases were retained.

## Local closure remediation (not a new campaign)

The missing import was reproduced locally against the exact candidate image
before any remote source mutation. A new unsubmitted candidate was then
created at `results/spec167-source-012-corrected-candidate/`; it adds only the
existing `Experiments/spec164_artifact_campaign.py` runtime dependency and
keeps the SIF, schedule, payload, and all source-012 code changes unchanged.
Its 16-file checksum list and runtime-closure validator pass. The candidate
also passes the exact local gate: 9 runner tests, 8 contract tests, the
two-container NFD preflight, and all four repository formal smoke subjects.

This is a local packaging correction only. Remote source-012, job 181820, its
`.partial` evidence, and the one-submission freeze remain immutable; no
replacement TigerCluster campaign was submitted.

The corrected candidate's local receipt is
`results/spec167-source-012-corrected-candidate/corrected-candidate-receipt.json`.
It records 16 checksum-bound files, the unchanged 60-row schedule
(`scheduleSha256=3073f60cd34846d8ce37570ab40e6ea445790526b681a6419805183740c0717c`),
and the reused Spec 166 SIF digest. The receipt is explicitly marked
`LOCAL_CORRECTED_CANDIDATE_NOT_SUBMITTED` and is not remote campaign evidence.

## Later closure boundary

This source-012 freeze remains immutable. After a separate explicit
authorization, the corrected closure was staged under the new source-013
identity and submitted once; its result is recorded in
`evidence/source-013-freeze.md`. This does not alter source-012's failure or
turn it into transport evidence.
