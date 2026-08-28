# T221 Dynamic-Plan Durable Request Identity

Date: 2026-07-15  
Verdict: **PASS**

CodeGraph tracing showed that canonical `APPClient.infer_async()` forwards its
durable request ID through the network submitter, but the internal runtime
facade's dynamic-plan `infer_async()` signature did not accept `request_id`.
The predeployed-service path already forwarded it correctly. A real dynamic
provisioning call would therefore raise `TypeError` before network submission.

The facade now accepts and forwards `request_id` to
`DistributedInferenceClient.infer_async()`. A regression constructs the actual
facade adapter and verifies the same ID reaches the underlying client with the
bound plan and unchanged timeouts.

Commands:

```bash
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 tests/python/test_ndnsf_di_app_sdk_compatibility.py
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 tests/python/test_ndnsf_di_request_handle.py
```

Results: **9/9 PASS** in 0.404 seconds and **14/14 PASS** in 0.180 seconds.
No MiniNDN or performance cell was run for this localized caller repair.
