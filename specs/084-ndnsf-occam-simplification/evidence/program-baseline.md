# Program Baseline

- Captured: 2026-07-10 America/Chicago
- Branch: `Experimental`
- HEAD: `2235fddc63863c644afa55065cf0098ba795d2bd`
- Command: `git status --short`
- Rollback boundary: child work starts from an explicit commit created after
  the pre-existing dirty changes are assigned or committed; Spec 084 does not
  authorize resetting them.

## Pre-Existing Dirty Ownership

Treat every path present before Phase 1 as user-owned/pre-existing:

- repository configuration: `.gitignore`, `.specify/feature.json`;
- current Repo/Core work: `Experiments/NDNSF_DistributedRepo_Generic_Minindn.py`,
  `NDNSF-DistributedInference/ndnsf_distributed_inference/repo.py`, the
  `NDNSF-DistributedRepo/` C++/Python/config/docs paths reported by Git,
  `ndn-service-framework/ServiceUser.*`, `ServiceProvider.cpp`,
  `pythonWrapper/ndnsf/{__init__,service}.py`, `_ndnsf.cpp`, and
  `generic-dynamic-api-targeted.t.cpp`;
- pre-existing untracked Repo experiments/tests/docs and Specs 071-083;
- deleted `tools/ai/deepseek_delegate.py`.

Phase 1 owns only:

- `specs/084-ndnsf-occam-simplification/`;
- `tools/maintenance/ndnsf_occam_audit.py`;
- `tests/python/test_ndnsf_occam_audit.py`.

Excluded from every child commit unless explicitly reassigned: proposal slides,
NDN-SVS, local identity/certificate state, secrets, `results/`, and every
pre-existing dirty path above.
