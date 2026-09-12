# Spec184 Qwen3-0.6B local MiniNDN r05 and layered runner revision

## Result boundary

本记录保留一次真实 root MiniNDN 尝试及其后的入口修正。r05 使用当前候选
`qwen06b-1e404007db8e`，candidate digest 为
`sha256:1e404007db8eb0063dd2339b7cda1facec980eac4cd2c214a0e53f322b4d3993`，运行目录为
`/tmp/spec184-qwen06b-layered-run/qwen06b-experimental-r05`。Controller、Authority 和三个
Provider 均到达 ready 标记；失败首边界是 User 的 C++ catalog source 解析：
`requester-0.log` 为 `NATIVE_REQUESTER_FAILED: DI_NATIVE_ONNX_PARSE`。没有 ACK/Selection/
Response、ONNX 数值或会话结果，不能计为协议或模型 PASS。MiniNDN teardown 后按 run identity
盘点无残留进程。

该失败证明旧脚本把 stage manifest 的元数据 JSON 写成了
`requester/model-source.json`，它不是 canonical ONNX protobuf。入口现已改为显式接收
`--canonical-source`，可选 `--canonical-initializer`；缺失、超限或非 ONNX source 在
`model` 层 fail closed，不再启动 MiniNDN。stage `stage-*.onnx` 仍只作为 role artifacts，
不能替代 canonical source。

## Layered validation

入口仍为 `check -> prepare -> run`，但 `prepare` 现在额外写出
`bundle-manifest.json`，其中包含 candidate digest、精确 child command 和 command digest。
`run` 在启动前核对该 bundle；启动标记完整而 Requester 失败时，外层记录为
`minindn=PASS`、`workload=FAIL` 并保留首个 C++ marker，区分运行时请求失败和 MiniNDN
启动失败。

当前验证：

- `python3 -m pytest -q tests/python/test_spec184_qwen06b_local_experiment.py`：`9 passed`。
- `python3 -m py_compile`（两个实验入口及 focused test）：`PASS`。
- 无 source 的 `check`：`machine=PASS`、`model=WAITING_EXTERNAL_INPUT`，退出 `1`，未启动 MiniNDN。
- `tests/fixtures/spec182/qwen-native-config.onnx` 的 `check` 与 `prepare`：source ONNX
  解析 `PASS`，bundle manifest 写入成功；该 fixture 只有 7 个节点，不能冒充真实 0.6B
  权重实验。

## Coverage retrospective

| Lane | Result |
| --- | --- |
| `static` | `PASS`：source 身份、bundle command digest、startup/workload 分层和 cleanup marker 已审查 |
| `compile-link` | `PASS`：Python syntax；native binary closure 由既有 candidate preflight 记录 |
| `runtime-test` | `PASS`：9 个入口 focused tests；r05 的真实启动边界已保存 |
| `unobserved` | canonical 0.6B source、真实 ACK/Selection/Response、模型数值、两轮会话仍未观测 |

T007、A3、A4 和 T008 状态不因本记录提升；canonical source 交付后才可重新执行
`prepare -> run`，并继续沿当前候选身份记录结果。
