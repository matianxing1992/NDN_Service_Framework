# Spec184 Qwen3-0.6B local experiment layers

## Scope and status

本记录只覆盖本机实验入口的分层整理，不代表一次 MiniNDN 协议或模型资格运行。业务行为仍由
C++ Controller、Authority、Provider、Requester 和 ONNX Runtime 执行；Python 只保存输入身份、
构造 APP bundle、启动既有 native runner 并收集证据。Qwen3.6-27B 仍为
`WAITING_EXTERNAL_INPUT`，T007 总体保持 `IN_PROGRESS` / `PARTIAL`。

## Layered entry point

入口为 `Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py`，profile 为
`Experiments/profiles/ndnsf-di-qwen06b-local.example.json`。七层出口固定为：

`machine -> candidate -> model -> bundle -> minindn -> workload -> cleanup`

`check` 只做 machine/candidate/model 预检；`prepare` 创建不可覆盖的 run 目录，写入
`preflight.json`、`app-manifest.json`、`launch.json` 和全层 `NOT_EVALUATED` 的 `run-record.json`；
`run` 只执行保存的完整命令，并在执行前重新核对 candidate digest。子 runner 的非零退出、缺失
child record 或 child record 非 `PASS` 都不会提升 workload。结束时按本次 run 的绝对目录扫描进程，
`PROCESS_CENSUS_FAILED` 或 `OWNED_PROCESS_ALIVE` 时 cleanup 不得 `PASS`。

## Static review and focused validation

按官方 `review-agent` 的只读覆盖线复核了输入契约、candidate/app-manifest 身份绑定、精确 launch
命令、C++ binary `readelf -d`/`ldd -r` 检查、状态机的 `NOT_EVALUATED` 传播、子进程所有权和
cleanup 盘点；未发现新的静态缺口。新增 focused tests 覆盖 profile unknown field、单一 native
build boundary、缺失 binary、canonical candidate digest 和自身进程排除，共 `5 passed`。

## Recorded checks

1. `python3 -m pytest -q tests/python/test_spec184_qwen06b_local_experiment.py`：`5 passed in 0.18s`。
2. `python3 -m py_compile Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py tests/python/test_spec184_qwen06b_local_experiment.py` 以及
   `python3 -m py_compile Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py`：`PASS`。
3. `git diff --check`（本批脚本、profile、文档和测试）：`PASS`。
4. `check` 使用当前 `build-spec184-b5-candidate-r4`、三阶段真实 Qwen3-0.6B ONNX artifact 和
   tokenizer：machine 与五个 native binary 的 `readelf`/`ldd -r` 均 `PASS`；candidate 为
   `qwen06b-c440dadca626`，digest 为
   `sha256:c440dadca6267b1a50d61f026107eac6a48481e650a05e71660d8db35f527b36`。
5. `prepare` smoke 写入 `/tmp/spec184-qwen06b-layered-layout-r2/qwen06b-layered-r02/`，
   preflight 为 `PASS`，launch 和七层 run record 保持 `NOT_EVALUATED`；本轮未运行 `run`，未
   启动 MiniNDN、SIF、远程机器或 27B 模型。

## Coverage retrospective

| Lane | Result |
| --- | --- |
| `static` | `PASS`：profile、状态传播、候选绑定、cleanup 盘点已审查 |
| `compile-link` | `PASS`：Python syntax；native binary closure 在 preflight 中 `PASS` |
| `runtime-test` | `PASS`：5 个编排器 focused tests；真实 MiniNDN workload 本轮未执行 |
| `unobserved` | `run` 后的真实 ACK/Selection/Response、两轮会话、模型数值、root cleanup 仍未观测 |

该单元是 `T007` 的本地实验编排准备出口，不能把 `T007-A3`、`T007-A4` 或 `T008` 改为完成。

## Revision after first root attempt

随后一次 root MiniNDN r05 到达 Controller、Authority 和三个 Provider 的 ready 边界，但
Requester 在 catalog source 解析处返回 `DI_NATIVE_ONNX_PARSE`。因此本入口已收紧为显式
canonical ONNX source 输入，并把 source 缺失/非 ONNX 检查前移到 `model` 层；stage artifact
不再被当作 source。`prepare` 另外写入 `bundle-manifest.json`，其 command digest 与
candidate digest 在 `run` 前复核。详细失败边界和当前 `PARTIAL` 状态见
[Qwen06B local r05 and layered runner revision](qwen06b-local-experiment-r05-20260912.md)。
