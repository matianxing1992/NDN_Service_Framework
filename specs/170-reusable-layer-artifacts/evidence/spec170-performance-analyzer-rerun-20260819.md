# Spec 170 performance-analysis tool rerun (2026-08-19)

This record verifies the analysis/contract code only. It is not performance
measurement evidence and does not close T036.

## Command

```bash
PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:Experiments \
python3 -m pytest -q \
  tests/python/test_spec168_cold_warm_remote_gate.py \
  tests/python/test_spec164_performance_analysis.py \
  tests/python/test_spec171_opportunity_holdout.py \
  tests/python/test_spec171_opportunity_holdout_descriptive.py
```

## Result

```text
22 passed in 1.51s
log sha256=3a74fd185c033bd896b83c24d7fb97a3ef13f8a64cf792957bafdc44293089d7
```

The regression covers cold/warm campaign contract validation, performance
verdict handling, matched opportunity-window parsing, and descriptive
holdout analysis. It confirms that the registered analysis path is runnable;
it does not provide the missing T036 three-block P01–P05 measurements,
hierarchical bootstrap, TOST, or Holm result.
