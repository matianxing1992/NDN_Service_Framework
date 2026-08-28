# T002 Immutable Campaign Harness Gate

**Status**: The retained V2 dry run below passed metadata identity/admission
behavior and started no experiment. Campaign V3 supersedes V2 for the current
candidate after exact-SIF job 182382 exposed an incomplete overlay identity.

## RED-first result

The new direct unittest entrypoint initially failed because
`spec168_campaign.py` did not exist. The host `/usr/bin/python3` has no pytest
module, so the project-compatible direct unittest command was used rather than
installing or modifying global Python packages.

## Implemented behavior

`jobs/spec168_campaign.py` now:

- requires eleven full SHA-256 bindings for baseline, source, SIF, bounded
  local fixture, remote-small/remote-large stage manifests, strategy, prompt
  set, route, analyzer, and schedule;
- verifies the SIF and remote stage-manifest bindings against the T001 baseline
  while keeping the local fixture content address independent;
- derives deterministic content-addressed candidate and campaign identities;
- atomically freezes experiment, schedule, and campaign manifests;
- orders focused, real MiniNDN, exact container, candidate audit, small single,
  small repeated, large single, and clean reproduction phases;
- sets `manualRemoteSubmissionOnly=true`, `automaticRetry=false`, and an
  at-most-once/no-auto-resubmit policy;
- rejects a duplicate campaign directory, mutated frozen manifest, missing or
  extra binding, baseline mismatch, and a second result writer;
- creates only a 326-byte `run-claim.json` when claiming a result root and
  copies no SIF or model payload.

## Verification

```text
python3 tests/python/test_spec168_campaign_contract.py
Ran 6 tests in 0.785s — OK
```

The retained real-baseline dry run used the T001 SIF/model identities plus
explicit harness/source/schedule inputs. It is a harness check, not a frozen
runtime candidate:

```text
CAMPAIGN_HARNESS_DRY_RUN_PASS
campaign_id spec168-campaign-v2-89ab9506f848026748f2
validated_id spec168-campaign-v2-89ab9506f848026748f2
campaign-manifest.json   2472 bytes  sha256:0340f2e6430ae433155566614a69be589407954f3e0b31d83b31227d914755af
experiment-manifest.json 1693 bytes  sha256:d9c11615c7672dee7db3e6facffccb7b3c46d91a80a5303092a61010a232f9c9
schedule-manifest.json   2436 bytes  sha256:f6121dbb01635dea46ab92029580eefdd12699db33a6eaf865bcda02e8e5aac8
claim_file_bytes 326
payload_files 0
automatic_submission False
```

Temporary generated manifests were deleted automatically after verification;
only this small summary and the input-binding evidence remain.

## V3 admission correction

Campaign V3 requires fourteen immutable bindings. In addition to V2's model
identity split, it binds the complete source/package-data bundle, the accepted
Gate B manifest, and the exact-SIF/CUDA Gate C result. Its ordered phases split
local `exact-container-overlay` from remote single-node
`exact-sif-cuda-preflight`. The latter is remote but is not the three-node
campaign. Six campaign contract tests pass for V3; the V2 identifiers above
remain historical dry-run evidence and are not rewritten.

The real-baseline V3 metadata dry run produced and validated
`spec168-campaign-v3-77e02f409b24b9e9daa8`. Its temporary manifests were
deleted after validation; retained input bindings are in
`campaign-harness-v3-dry-run-bindings.json`. The manifest SHA-256 values were:

```text
campaign  ff0da8d6f317dc997e33c80092df8d6095985f6b613fe60f2a73769bd82b77b6
experiment 962ba49a9d43507faea32253122682ecb69fe2cfa067cbbf0002d18c17722268
schedule  59c6538ebe8b14dc16d5dec0660dd9aeec8e469d8b39a16dbfd0ed580e83aad5
```

## Admission boundary

T002 does not authorize a three-node campaign. Gate B is satisfied by the v30
canonical tiny-Qwen run and Gate C by Slurm job 182384. Candidate audit must now
bind those exact results and must still reject remote execution until a native
Spec 168 three-node launcher/analyzer is frozen.
