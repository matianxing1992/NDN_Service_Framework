# R10-B7 Caller Route Contract Synchronization Evidence

**Date**: 2026-09-09
**Batch**: R10-B7
**Baseline**: `e639acac` (R10-B6 Provider negative boundary checkpoint)
**Scope**: the maintained Qwen route marker, T013-D/T013-F execution-unit wording, and the
Spec182 task registry. Historical R5 evidence and native source are intentionally unchanged.

## Behavior boundary

R10-B3/R10-B4 already moved maintained YOLO/Qwen native callers to encrypted repository
references. The Qwen startup record still printed `route=request_native_payload`, and the current
T013-D/T013-F contract text still described the old inline route. This documentation/observability
batch changes the marker and current contract wording to
`publish_application_input_reference` → `request_native_reference`; it does not change request
behavior, Python planner ownership, Provider ownership, or the legacy ACK-driven compatibility
path.

## Coverage matrix

| Lane | Status | Files / symbols | Query or check | Findings / re-review |
| --- | --- | --- | --- | --- |
| `production entry/callers` | `covered` | Qwen startup marker; `_native_qwen_request`; YOLO `_load_yolo_native_payload`; T013-D/T013-F contract | `rg -n "route=request_native_reference|request_native_reference|request_native_payload" examples/python/NDNSF-DistributedInference specs/182-native-di-python-bindings/contracts/execution-units.md` | Current maintained native routes are named as REPO_REF; remaining `request_native_payload` references are historical/generic facade documentation or compatibility APIs |
| `implementation and wire` | `covered` | `publish_application_input_reference`, `APPClient.request_native_reference`, v2 `REPO_REF` envelope | `rg -n "publish_application_input_reference|request_native_reference" examples/python/NDNSF-DistributedInference/llm_pipeline/user.py examples/python/NDNSF-DistributedInference/yolo_2x2/user.py NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py` | Contract wording matches the implemented encrypted repository-reference route; no wire or native implementation edit |
| `test/harness/oracle` | `covered` | `tests/python/test_spec182_legacy_exclusion.py`; existing R10-B3/R10-B4 source oracles | `python3 -m py_compile examples/python/NDNSF-DistributedInference/llm_pipeline/user.py examples/python/NDNSF-DistributedInference/yolo_2x2/user.py`; focused pytest source suite | Source route and syntax checks pass; no network behavior is inferred from this documentation batch |
| `build/source closure` | `N/A` | Python caller and Markdown contract only; native ABI unchanged | `git diff --check`; `validate_design.py` | No C++ target or generated binding changed; native rebuild is not applicable |
| `migration/evidence` | `covered` | R10-B3/R10-B4 evidence links; R5 historical records; T013-B retirement boundary | `rg -n "R10-B3|R10-B4|legacy|retirement" specs/182-native-di-python-bindings/{plan.md,tasks.md,contracts/execution-units.md}` | Historical inline evidence remains dated and linked; current docs no longer mislabel the active reference route; real caller execution and retirement remain open |

## Review trace

- Official read-only skill: `/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
  `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`.
- Review scope: baseline `e639acac` to the marker, execution-unit, plan, task-registry and
  evidence diff. The review explicitly separated current route text from historical R5 records
  and generic `request_native_payload` facade API references.
- No actionable finding remained after re-review. The shared Spec Kit five-lane Minimum Review
  Record applies; because no native source or target changed, the build lane is explicitly `N/A`.

## Validation

```text
python3 -m py_compile \
  examples/python/NDNSF-DistributedInference/llm_pipeline/user.py \
  examples/python/NDNSF-DistributedInference/yolo_2x2/user.py
exit=0

python3 -m pytest -q \
  tests/python/test_spec182_legacy_exclusion.py \
  tests/python/test_ndnsf_di_app_sdk_compatibility.py
exit=0
```

`git diff --check` exited 0. `python3 specs/182-native-di-python-bindings/checklists/validate_design.py`
reported `errors: []`; the validator's `runtime_tests` and `product_static_review` fields remain
`NOT_RUN` by design and are not promoted by this batch.

## Batch retrospective

- `static`: stale route wording was found and corrected; current/historical/generic references
  were reclassified explicitly.
- `compile/link`: `none`; no native source, target, or ABI changed.
- `runtime/test`: source syntax and compatibility tests passed; no network request was run.
- `unobserved`: maintained Qwen/YOLO Core/Provider execution, stream callback delivery,
  cross-process behavior, legacy zero-use, and T016 qualification remain open.
- `batch expansion`: limited to one marker and two execution-unit descriptions; no product
  behavior or new dependency was added.

## Closure decision

`CLOSED_FOR_VALIDATION` for the caller route contract/observability synchronization. The active
documentation and emitted marker now identify the encrypted repository-reference path consistently
with R10-B3/R10-B4. T013-A/B/C/D/F and T016 remain `PARTIAL`/open until real caller execution,
cross-process streaming, retirement evidence, and qualification are observed.
