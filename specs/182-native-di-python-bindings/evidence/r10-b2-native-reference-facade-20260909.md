# R10-B2 Native REPO_REF Facade — 2026-09-09

## Scope and allocation

本批只把已经由 `publish_application_input_reference` 产生并写入 requester
journal 的 `LargeDataReference` 接入公开 native facade。入口为
`APPClient.request_native_reference` → `APPClient.request_native` →
`NativeInferenceClient::request`。facade 只校验发布摘要、生成 canonical reference
JSON 并设置 `NativeInputTransportMode::RepositoryReference`；它不解析仓库、不解密
数据、不调用 Python planner，也不改变 Provider 的 fetch/decrypt owner。

## Coverage matrix

| Lane | Evidence |
| --- | --- |
| `production entry/callers` | `APPClient.request_native_reference`, `APPClient.request_native`, public `InferenceClient.request_native_reference`; existing publication owner `publish_application_input_reference` |
| `implementation and wire` | `LargeDataReference.from_mapping`/`digest`, journal `referenceDigest` binding, canonical `to_dict()` JSON, pybind `NativeApplicationInput.transport_mode` and `repository_reference` |
| `test/harness/oracle` | `test_core_native_reference_route_preserves_identity_and_avoids_planner`; `test_public_inference_client_forwards_native_reference_route`; related Spec180/Spec182 Python selectors |
| `build/source closure` | `NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py`, `tests/python/test_ndnsf_di_app_sdk_compatibility.py`; native ABI reused from R10-B1 and no C++ source changed |
| `migration/evidence` | published-reference facade is available to maintained Python callers; requester-side planner fallback and decrypt are absent; real encrypted fetch, Provider execution, caller migration, cross-process behavior and T016 remain open |

## Static review

The read-only review used `/home/tianxing/.codex/skills/review-agent/SKILL.md`
(SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`)
against the complete R10-B2 diff, the two facade call paths, the publication journal
record shape, and the pybind field names. The review checked publication binding,
canonical serialization, planner bypass, observer forwarding, options ownership,
and the no-decrypt boundary. **No actionable findings.**

## First-boundary failure and changed check

The first focused test invocation reached the new positive selector but failed in
the test double with `AttributeError: 'FakeInput' object has no attribute 'payload'`.
The C++ binding supplies default empty `payload` and `options`; the fake did not model
those defaults. This is a harness-only failure, not a product or protocol result.
The fake now initializes both fields, and the failure index records the boundary in
`docs/failure-log.md`. The retry output is retained at
`.codex-tmp/spec182-r10-b2/app_sdk.log`.

## Validation

```text
python3 -m py_compile app_sdk/client.py test_ndnsf_di_app_sdk_compatibility.py  exit=0
pytest test_ndnsf_di_app_sdk_compatibility.py                             17 passed
pytest test_spec180_generic_request_api.py test_spec182_native_bindings.py \
       test_spec182_legacy_exclusion.py                                   35 passed
git diff --check (scoped R10-B2 paths)                                    exit=0
validate_design.py                                                        ok=true
```

No C++ rebuild was needed because the batch changes only the Python facade and
reuses the R10-B1 native ABI. No network, MiniNDN, cross-process, or Provider
encrypted-fetch run was attempted; those gaps remain explicit.

## Batch retrospective

- Static review found: no actionable code defect; the review explicitly covered the
  five required lanes and the caller/publication ownership boundary.
- Compile/static checks found: no source or syntax miss after the test-double fix.
- Runtime/test found: one harness-default miss on the first attempt; the retry and
  related selectors passed (17 + 35 cases).
- Still unobserved: native requester to Provider encrypted fetch/decrypt, real
  maintained-caller migration, cross-process behavior, MiniNDN and qualification.

`Closure decision`: `CLOSED_FOR_VALIDATION` for the local Python reference-facade
boundary; `T004/T010/T013/T016` and final qualification remain `OPEN_FOR_NEXT_BATCH`.
