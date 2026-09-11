# R11-B8-G45 Tiger Y-B Native and Topology Binding

**Status**: `CLOSED_FOR_VALIDATION` for the bounded renderer/dispatcher contract
**Parent**: R11-B8 / T013 maintained caller boundary
**Date**: 2026-09-10

## Finding and repair

静态追踪 `run-functional.sh → render-tiger-yb-args.py → yolo_2x2/user.py` 发现：renderer
原先传递 `--native-tensor-input`，却没有传递 `--native-requester-config`。`user.py` 只有在
后者存在时才进入 `_load_yolo_native_payload`；因此 Tiger workload 会悄悄执行历史
ACK-driven Python planner，MiniNDN 的 native caller 检查不能代表 Tiger 路径。

同一追踪还发现 Y-B case config 可声明多个 `runtime.nodes`，但 Tiger profile 是
`--nodes=1`，supervisor 在同一 task 内启动一个 NFD 和全部四个 Provider。原 renderer
没有消费 topology 或检查 node map，可能把未执行的多机 placement 记录成成功。

本批修改：

- `YOLO_ENVIRONMENT_FIELDS` 和 renderer workload contract 增加
  `SPEC180_YOLO_NATIVE_REQUESTER_CONFIG`；renderer 要求 config 文件存在，并将其传给
  User 的 native route。
- native User argv 移除当前 native branch 会拒绝的 `--request-id`、lifecycle 参数和
  旧 sequential marker。
- renderer 要求 topology 文件存在，记录其 digest；若 `runtime.nodes` 有多个不同
  节点，先返回 `SPEC180_TIGER_RENDER_MULTI_NODE_RUNTIME_UNSUPPORTED`。
- render manifest 明确记录 `deploymentScope=single-node-native-requester`。
- local inventory 的 input identity 现在同时绑定 native requester config 的文件摘要，
  因此提交侧、渲染侧和执行侧不能悄悄使用不同配置。

这使不支持的多机 Tiger 路径在子进程启动前失败，而不会把 MiniNDN 拓扑或 Python fallback
误报为 C++ 多机能力。当前 Spec180 collector 仍只消费 legacy lifecycle/numerical evidence，
所以 native Tiger 的完整结果桥接仍需后续 caller/qualification 工作。真正的跨节点执行仍
需 Spec110 allocation topology launcher 和 T016 的 C++/no-Python qualification。

## Validation

| Check | Result |
| --- | --- |
| `python3 -m pytest -q tests/python/test_spec180_dispatcher.py tests/python/test_spec180_release_workflow.py tests/python/test_spec180_tiger_contract.py` | 39 passed |
| `python3 -m pytest -q tests/python/test_spec180_inventory.py tests/python/test_spec180_dispatcher.py tests/python/test_spec180_release_workflow.py tests/python/test_spec180_tiger_contract.py tests/python/test_spec182_legacy_exclusion.py tests/python/test_spec180_qwen_entrypoint.py tests/python/test_spec166_itiger_job_sources.py` | 97 passed |
| `python3 -m py_compile scripts/run_spec180_case.py packaging/ndnsf-di-container/jobs/spec180/render-tiger-yb-args.py Experiments/TigerCluster/jobs/spec180/render-tiger-yb-args.py` | PASS |
| `bash -n packaging/ndnsf-di-container/jobs/spec180/run-functional.sh Experiments/TigerCluster/jobs/spec180/run-functional.sh` | PASS |
| `python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py --strict specs/182-native-di-python-bindings` | PASS |
| `python3 skills/speckit-code-design/scripts/verify-spec-kit-sync.py --repo-root . --require-entrypoints` | PASS |
| `git diff --check` | PASS |

No C++ rebuild was required: this unit changes only the sealed workload dispatcher/renderer,
tests and design evidence. No real Tiger/SIF/Slurm multi-machine or T016 qualification was run.
