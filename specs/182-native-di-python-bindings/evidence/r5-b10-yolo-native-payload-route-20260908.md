# R5-B10 Maintained YOLO Native Payload Route

日期：2026-09-08。此批次只推进 `T013-A` 的维护入口接线，不运行网络、MiniNDN、SIF 或
Tiger qualification。

## Scope and Boundary

`examples/python/NDNSF-DistributedInference/yolo_2x2/user.py` 新增显式
`--native-requester-config` 分支。该分支先调用 `APPClient.configure_native_requester_from_config`，
再把 `encode_native_tensor_bundle` 产生的 inline bytes 交给
`APPClient.request_native_payload`；native model adapter、task、runtime、grant/admission、
split/placement 和请求状态仍由 C++ owner。分支不会调用 `configure_automatic_planning` 或
`request_task`，并在缺少 native tensor、包/registry、生命周期参数、模型身份或 native 结果
时 fail-closed。Spec180 lifecycle journal 暂不伪造：若传入其 request/case/output 参数，直接
拒绝并保持未验收。

## Review trace

- Skill: `/home/tianxing/.codex/skills/review-agent/SKILL.md`
- SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`
- Baseline: `65da21f5` (R5-B9 checkpoint)
- Diff: YOLO maintained User, `test_spec182_legacy_exclusion.py`, execution-unit card and
  `tasks.md` R5-B10 progress/evidence links
- Coverage: branch ordering, native configuration boundary, model/package identity binding,
  inline payload and task/options mapping, result decoding, exception/shutdown ownership,
  Python planner reachability, lifecycle evidence boundary
- Findings: 初始实现把输入 fixture 对象误传给数值比较器；静态复核发现后改为
  `compare_reference`。同时发现 native request 当前由 C++ 分配 request identity，不能安全
  写入 Spec180 `LifecycleJournal`，因此加入显式拒绝而不是写入不完整记录。最终复核无未处理
  的控制性发现。

## Validation

```text
python3 -m py_compile examples/python/NDNSF-DistributedInference/yolo_2x2/user.py  # exit 0
PYTHONPATH=pythonWrapper:NDNSF-DistributedInference python3 -m pytest -q \
  tests/python/test_spec182_legacy_exclusion.py                                  # 5 passed
git diff --check                                                               # exit 0
```

本批次没有修改 C++ 或 Python extension，因此没有重新构建 native target；R5-B9 的 shared
library/extension source-closure 身份继续作为前置记录，不能替代本批真实请求验证。

## Result

`STATIC_PASS`; `FOCUSED_BEHAVIOR_PASS`; `BUILD_NOT_APPLICABLE`; `PARTIAL`; not
`QUALIFICATION_PASS`。真实 Core/Provider 请求、numeric parity、Spec180 lifecycle、默认
Python route retirement、T013-B 和 T016 仍开放。
