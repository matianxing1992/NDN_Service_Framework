# Spec170 performance claim-boundary rerun (2026-08-19)

The performance analyzer and paper-claim guard were rerun after the current
evidence audit. This is a tooling/interpretation regression, not a new
performance campaign.

```text
command: PYTHONPATH="$PWD" python3 -m pytest -q \
  tests/python/test_spec173_paper_submission_evidence.py \
  tests/python/test_spec164_performance_harness.py \
  tests/python/test_spec171_opportunity_holdout.py \
  tests/python/test_spec171_opportunity_holdout_descriptive.py \
  tests/python/test_spec168_cold_warm_remote_gate.py \
  tests/python/test_spec164_performance_analysis.py
return code: 0
result: 40 passed in 2.30 s
log sha256: a9d1bef43ba9966c410e31a7bde92e3f1c1161386776057f9d73c109fc0c28ba
```

The guards continue to reject unsupported optimum/significance claims from
descriptive, smoke, or incomplete artifacts. The rerun does not create the
missing T036 three-block P01–P05 corpus, hierarchical bootstrap, TOST, or Holm
analysis; performance optimality therefore remains unverified.

The retained opportunity holdout analyzer was also rerun against the original
content-addressed campaign directory (no new network campaign):

```text
verdict=HOLDOUT_CONFIRMS_CONDITIONAL_END_TO_END_ADVANTAGE
switch_required_requests=1312
transition_boundary_requests=28
state_disagreement_requests=7
```

The fresh output digests were `holdout-requests.csv`:
`4fb7fc7c0aa75c9eb765fe966ed1070a441e8ff76b13f82fe8477d5f2ecb7700`, and
`holdout-summary.json`:
`efd9dec6c4a8e5c78b386f1bab12323c47beb09c660c123f2e090f178412bd38`.
This reproduces the conditional holdout result and does not change its
scope: it still cannot establish a global optimum or close T036.
