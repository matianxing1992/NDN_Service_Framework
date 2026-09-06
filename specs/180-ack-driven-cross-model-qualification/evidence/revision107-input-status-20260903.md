# Revision 107 input-status evidence

Date: 2026-09-03
Status: implementation correction; G0/T011-A remains open.

The YOLO runner now has two explicit result classes:

- `WAITING_EXTERNAL_INPUT`, exit 78: `validate_inputs()` rejected missing or
  invalid candidate/configuration inputs before NFD/SVS startup.
- `UNQUALIFIED`, exit 2: `run_minindn_case()` began and then failed during
  protocol execution or cleanup.

Verification:

```text
pytest -q tests/python/test_spec180_yolo_minindn.py \
  -k 'missing_g0_inputs or main_never_emits_pass'
2 passed

python3 Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-A
SPEC180_CASE_RESULT status=WAITING_EXTERNAL_INPUT error=ENVIRONMENT_MISSING:...
exit 78
```

The direct probe had no candidate, key, model, topology, or policy inputs, so
it correctly stopped before MiniNDN side effects. This evidence changes only
classification; the complete Spec180 Python collection also passed with
`184 passed, 22 warnings`. It is not an NDNSF-DI protocol result and does not
close G0.
