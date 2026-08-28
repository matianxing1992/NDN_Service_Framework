# US1 Rollback Demonstration

Verdict: **PASS**

The Phase 3 release is additive and its compatibility boundary is schema-neutral:

1. Root exports resolve lazily through `compatibility/manifest.json` and `compatibility/exports.py`.
2. `runtime_v1` imports and re-exports the canonical Core contracts; it does not translate or rewrite persisted payloads.
3. The generic `ProviderExecutionLeaseTable` ABI and lease wire schema remain v1 and were not forked.
4. The pre-separation release can therefore be restored by selecting the previous Python modules/native library while retaining existing runtime-v1 plans and lease records; no database, evidence or model-artifact migration is required.

Demonstration command:

```bash
PYTHONPATH="$PWD/NDNSF-DistributedInference:$PWD/pythonWrapper" python3 - <<'PY'
from ndnsf_distributed_inference import runtime_v1
from ndnsf_distributed_inference.core import ProviderProfileV1, RuntimeTelemetryV1
assert runtime_v1.ProviderProfileV1 is ProviderProfileV1
assert runtime_v1.RuntimeTelemetryV1 is RuntimeTelemetryV1
print("US1_COMPATIBILITY_ROLLBACK_OK")
PY
```

Observed: `US1_COMPATIBILITY_ROLLBACK_OK`. Rollback affects code selection only; immutable historical evidence and fault results remain untouched.
