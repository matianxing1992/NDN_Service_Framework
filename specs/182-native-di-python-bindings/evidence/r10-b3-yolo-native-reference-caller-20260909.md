# R10-B3 YOLO Native Reference Caller — 2026-09-09

## Scope and allocation

本批只迁移维护入口 `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py`
的 `_load_yolo_native_payload`。它继续使用同一 native requester configuration、模型
身份校验和 tensor bundle；变化是先调用 `APPClient.publish_application_input_reference`
发布加密对象，再把返回的 journal-bound reference 交给
`APPClient.request_native_reference`。Provider 负责后续 fetch/decrypt。ACK-driven
`request_task`、生命周期负例、Python planner 及其它调用方均未改变。

## Coverage matrix

| Lane | Evidence |
| --- | --- |
| `production entry/callers` | `_load_yolo_native_payload` after `configure_native_requester_from_config`; `main` selects this branch when `--native-requester-config` is set |
| `implementation and wire` | one `publish_application_input_reference` call followed by `request_native_reference`; no inline `request_native_payload` remains in the native branch |
| `test/harness/oracle` | `test_maintained_yolo_native_route_uses_repository_reference`, existing `test_maintained_yolo_native_route_is_explicit_and_fail_closed`, and R10-B2 facade selectors |
| `build/source closure` | `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py`, `tests/python/test_spec182_legacy_exclusion.py`; Python ABI surface from R10-B2/R10-B1 reused |
| `migration/evidence` | native YOLO caller now reaches the repository-reference boundary; ACK-driven planner and lifecycle branches remain explicitly separate; real MiniNDN Provider fetch, cross-process behavior, and T016 remain open |

## Static review

The read-only review used `/home/tianxing/.codex/skills/review-agent/SKILL.md`
(SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`)
against the complete caller diff and the `APPClient` reference-facade call path. It
checked branch selection, publication ordering, input identity, planner bypass,
legacy branch isolation, and Provider fetch/decrypt ownership. **No actionable
findings.**

## Validation

```text
python3 -m py_compile yolo_2x2/user.py test_spec182_legacy_exclusion.py   exit=0
pytest test_spec182_legacy_exclusion.py test_ndnsf_di_app_sdk_compatibility.py  24 passed
source branch checks (publish-before-reference/no-inline/no-planner)        exit=0
git diff --check (scoped R10-B3 paths)                                      exit=0
validate_design.py                                                          ok=true
```

No C++ rebuild was needed: the batch changes only a Python maintained caller and
uses the already validated R10-B2 facade/R10-B1 native ABI. No network, Provider,
cross-process, MiniNDN, or qualification run was attempted.

## Batch retrospective

- Static review found: no actionable control or ownership defect; all five lanes
  and the native/legacy branch boundary were covered.
- Compile/link found: no compile miss; Python syntax and scoped design checks pass.
- Runtime/test found: 24 focused compatibility/legacy cases pass; source oracle
  confirms publication precedes reference submission and the native branch cannot
  call inline native payload or `request_task`.
- Still unobserved: real encrypted fetch/decrypt, Provider execution, cross-process
  caller behavior, and T016 qualification.

`Closure decision`: `CLOSED_FOR_VALIDATION` for the local YOLO native caller
migration; `T004/T010/T013/T016` and final qualification remain `OPEN_FOR_NEXT_BATCH`.
