# R10-B60 Native Qwen Launcher Contract 2026-09-09

## Scope

本批修复维护中的 Qwen wrapper → LlmPipeline runner → User 入口之间的
native requester 配置接线。此前 wrapper 添加了
`--native-requester-config`，但 runner 没有 parser 或 User 转发；同时 wrapper
固定选择 `qwen-onnx`，因此不能启动 native Provider。该批只修复入口契约，
不把局部 parser/命令检查升级为真实请求资格。

## Coverage matrix

| Lane | Status | Scope and check |
| --- | --- | --- |
| production entry/callers | covered | `Experiments/NDNSF_DI_QwenAckDriven_Minindn.py::build_delegate_argv/run_from_environment`; `Experiments/NDNSF_DI_LlmPipeline_Minindn.py::main`; maintained `llm_pipeline/user.py` command contract; exact `rg` and CodeGraph exploration |
| implementation and wire | covered | wrapper selects `qwen-onnx-cpu-native` when native config is present; runner parses and validates the config, emits it in `native_user_args`, supplies the Qwen runtime summary, and excludes automatic-planning arguments |
| test/harness/oracle | covered for contract, gap for runtime | `tests/python/test_spec180_qwen_entrypoint.py` native delegate/parser test; existing native binding and legacy exclusion tests; 27/27 Python tests. No real Provider or MiniNDN request in this batch |
| build/source closure | N/A | Python/launcher-only change; no C++ source, shared library, executable target, or ABI changed; no native rebuild required |
| migration/evidence | covered with remaining gap | Native config now has one maintained launcher route and fails closed for missing config; Qwen real Provider/cross-process, continuation owner, legacy retirement and T016 remain open |

## Static findings

初始审计发现两个相互关联的接线缺陷：

1. Qwen wrapper 的 `build_delegate_argv` 传递了
   `--native-requester-config`，但 LlmPipeline parser 不接受该参数。
2. wrapper 固定使用 `qwen-onnx`，并且 runner 的 User 命令只在
   `qwen-onnx-cpu-native` 分支加入 native Provider 参数；即使 parser 补齐，
   仍会落到 Python Provider/规划路径。

修复内容：

- native config 存在时 wrapper 选择 `qwen-onnx-cpu-native`；普通路径继续选择
  `qwen-onnx` 并保留 `--selection-dataflow-v3`。
- runner 增加 config parser、文件存在性和 runtime 约束；native config 与
  Selection Dataflow 互斥，避免 User 同时收到 planner manifest。
- runner 将 native config 转发给 User，并在该路径提供
  `qwen-pipeline-runtime.json`，使 User 的 native branch 能构造 Qwen context。
- 增加 wrapper native delegate、runner parser 和 planner 参数排除回归。

复审范围覆盖完整 diff、wrapper 唯一 caller、runner provider/user command
构造、User parser 和既有 Qwen/legacy tests；复审未发现新增控制性缺陷。

## Compile/build misses

`none`。本批不改变 C++、Waf target、Python extension 或 ABI，因此没有执行
构建。`python3 -m py_compile` 通过；此前 `pyflakes` 仍不可用作 Python 3
检查，因为系统命令为 Python 2.7 版本。

## Runtime/test misses

本批没有启动 MiniNDN、native Provider 或跨进程 requester。测试只证明 argv/parser
契约和既有 Python facade/config 边界；真实 `requester → Core → Provider`
结果、stream observer、continuation、receipt/control/commit/recovery 仍未观测。

## Build measurement

`N/A`（Python/launcher-only；无 C++ build）。

## Behavior result

- `STATIC_PASS`：wrapper/runner/User 接线复审。
- `FOCUSED_BEHAVIOR_PASS`：`python3 -m pytest -q tests/python/test_spec180_qwen_entrypoint.py tests/python/test_spec182_native_bindings.py tests/python/test_spec182_legacy_exclusion.py`，27 passed；`py_compile` 与 `git diff --check` 通过。
- `QUALIFICATION_PASS`：未执行。
- 批次状态：`PARTIAL`。

## Review trace

- Review skill: `/home/tianxing/.codex/skills/review-agent/SKILL.md`
- Review skill SHA-256: `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`
- Static workflow reference: `/home/tianxing/.codex/skills/speckit-code-design/references/pre-test-static-review.md`
- Baseline: `fa1935b5c186f1fb81f39c020bc449fe4623b2292`
- Diff scope: `Experiments/NDNSF_DI_QwenAckDriven_Minindn.py`, `Experiments/NDNSF_DI_LlmPipeline_Minindn.py`, `tests/python/test_spec180_qwen_entrypoint.py`
- Queries/checks: CodeGraph exploration of `build_delegate_argv`, LlmPipeline `build_parser/main`, native Provider branch and User command; exact `rg` caller scan; parser/argv contract assertions; focused pytest; `py_compile`; `git diff --check`。
- Findings and re-review: initial parser/runtime disconnect repaired; second read-only review found no new P1/P2/P3 issue within this diff.

## Batch growth decision

本批在发现稳定的 launcher contract 出口后停止扩张，没有加入 Provider 状态机、
conversation owner 或 legacy retirement。那些职责拥有不同的生产入口、C++
selector 和运行资格依赖，留给后续 P2/P4/P5 批次。

## Closure decision

`OPEN_FOR_NEXT_BATCH`。本批达到的是 native-config Qwen launcher contract 的
静态与 focused Python 出口；尚未达到真实 native Provider 请求出口。下一步必须
在 owner 环境运行 native Provider 和 maintained Qwen User，先闭合 P1/P2，再
执行 Qwen stream/continuation 与 T016 资格。

## Batch retrospective

- `static`: 初始静态审计发现 parser 缺失、runtime 错选和 planner 参数冲突；已修复并复审。
- `compile/link`: `none`；本批无 C++ 构建，未覆盖 native source closure。
- `runtime/test`: focused tests 通过；真实 Provider/跨进程运行未执行。
- `unobserved`: native requester 配置内容、Core 授权、Provider worker、stream terminal、continuation recovery 和 no-Python 资格仍未观察。
