# T024 Proven Tiger Profile Implementation

**Date:** 2026-09-01  
**Status:** PASS at the implementation boundary; no Tiger allocation performed

## Problem closed

The former Spec175 submit path accepted ambient environment values and used
`sbatch --export=ALL`. The checklist verified evidence presence and hashes but
did not compare the effective command, working directory, resource request,
identity/mount contract, or helper bytes with one proven D0/r23 launch. A
syntactically complete checklist could therefore authorize a drifted wrapper;
retries reproduced the same defect under new job IDs.

## Implemented contract

The normative operator-facing contract is now maintained in
[`contracts/tiger-experiment-profile-v1.md`](../contracts/tiger-experiment-profile-v1.md)
so the profile, local replay, and iTiger skill use the same field ownership,
consumer, and retry rules. This closes the previous documentation gap in
which the right idea existed in several places but an operator could still
reconstruct a near-duplicate command.

- `packaging/ndnsf-di-container/jobs/spec175/proven-tiger-profile.json` is the
  versioned baseline for the five registered gates and hashes every helper,
  wrapper, Slurm job, and validator in the submission closure.
- `spec175_tiger_profile.py` validates the profile schema, tracked-file hashes,
  job directives/resource envelope, run schema/profile digest, and the gate
  allowlist. It records only the explicitly consumed stage-device mapping as an
  `ALLOWLISTED_DELTA`; Provider/GPU counts, seed, and resource envelope are
  fixed per gate and a changed value is rejected. Unknown, unsafe,
  control-character, or changed launch inputs are rejected.
- `submit.sh <gate> <profile> <run-record>` is the only public entry point.
  `submit_profile.py render` creates the candidate-bound delta report before
  any remote operation; `submit` re-renders and requires the report, effective
  configuration, sealed environment, and submitted argv to match exactly.
- The scheduler receives an explicit `--export=NONE,...` list. Stage-device
  IDs use a colon transport and are decoded only by the checked-in stage
  wrapper, so comma-delimited Slurm exports cannot corrupt them.
- The legacy ambient diagnostic wrapper is fail-closed and cannot allocate a
  node. The repository checklist now requires
  `proven-baseline-exact-delta`; the operator checklist and skill require the
  same report.

## Focused verification

```text
tests/python/test_spec175_tiger_profile.py                         7 passed
tests/python/test_spec175_tiger_checklist.py + test_spec175_sif_preflight.py 36 passed
profile render against the real five-gate job tree                  PASS
python -m py_compile (profile, submitter)                           PASS
bash -n (submit and Spec175 wrappers)                               PASS
```

The mutation set covers unknown run fields, helper-byte changes, job-directive
drift, profile/run digest mismatch, unsafe stage-device transport, a fixed-seed
delta, and a positive colon-delimited stage-device mapping. No SSH, upload,
model staging, `sbatch`, SIF build, or Tiger job was run by these tests.

## Restart boundary

Because the profile, submitter, checklist, and stage wrapper are tracked
candidate inputs, any change invalidates the source seal and downstream
evidence. The post-correction source seal and fixed-seed T020/T022 matrix now
pass; T023 is the next gate and must build one new exact-SIF candidate. No
historical SIF, job number, or changed-parameter retry can authorize Tiger.
