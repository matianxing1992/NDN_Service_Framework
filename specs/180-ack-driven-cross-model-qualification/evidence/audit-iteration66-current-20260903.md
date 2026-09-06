# Spec180 iteration-66 case-policy audit

Date: 2026-09-03

The runner now materializes `case-policy.json` inside the fresh case output
directory before any NFD/Provider startup. It copies the candidate-bound
authorization input, retains repository services, and filters the inference
service to the registered role set and explicit one-role Provider owners for
Y-A, Y-B, or Y-N. The source policy is not modified. The generated policy is
only a process-start authorization view; ACK closure remains the sole
request-time placement authority. Its SHA-256 digest is recorded in
`case-input.json`.

Verification:

```text
PYTHONPATH='NDNSF-DistributedInference:pythonWrapper' \
  python3 -m pytest -q tests/python/test_spec180_yolo_minindn.py
16 passed in 0.14s
```

At iteration 66, the Spec180 Python collection was 121 passed with 19
exporter/runtime warnings. The real NFD/NDN-SVS driver is still fail-closed,
so this audit does not create MiniNDN, SIF, or Tiger qualification evidence.
