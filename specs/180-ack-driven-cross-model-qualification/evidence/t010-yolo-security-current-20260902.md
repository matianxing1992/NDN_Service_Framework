# Spec180 T010 YOLO Security Boundary Evidence

**Status**: `PARTIAL_IMPLEMENTATION` (boundary regressions only)

## Focused command

```bash
python3 -m pytest -q \
  tests/python/test_spec180_generic_request_api.py \
  tests/python/test_spec180_yolo_security.py
```

Result: `19 passed`. The tests cover bounded inline/reference transport,
encrypted-reference scope, protection-epoch, and digest tampering, name/size-only repository
fetch rejection, non-owner input/result access, and single-use terminal
Response publication. They do not claim signed NDN repository execution,
native assignment verification, replay coverage, or a complete YOLO numerical
result.

The production path still requires T009/T011 wiring and the remaining T010
negative matrix (assignment, wrong Provider, replay, dependency, cache epoch,
deadline, cancellation, and redaction checks) before this task can close.
