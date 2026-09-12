# Spec184 本地 Qwen3-0.6B 实验分层流程

`Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py` 是本机实验的唯一编排入口。
它借鉴 TigerCluster 的 `check -> prepare -> run` 边界，但不构建 SIF，也不把本机
结果提升为 Qwen3.6-27B qualification。

## 入口

先复制并修改 [示例 profile](../Experiments/profiles/ndnsf-di-qwen06b-local.example.json)。
profile 只保存机器和 native 二进制输入；模型、轮数和 token 输入仍由命令行明确给出。

```bash
python3 Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py check \
  --profile Experiments/profiles/ndnsf-di-qwen06b-local.example.json \
  --stage-manifest .codex-tmp/spec184-qwen06b-native-20260912/qwen-onnx-service-manifest.json \
  --stage-root .codex-tmp/spec184-qwen06b-native-20260912/qwen-onnx-stage-artifacts

python3 Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py prepare \
  --profile <profile.json> --stage-manifest <manifest.json> --stage-root <stage-root> \
  --run-id qwen06b-local-r01

sudo -E python3 Experiments/NDNSF_DI_Qwen06B_LocalExperiment.py run \
  --profile <profile.json> --stage-manifest <manifest.json> --stage-root <stage-root> \
  --run-id qwen06b-local-r01
```

`local` 可组合 `prepare` 和 `run`，适合一次性开发 smoke；需要复现实验或排查
失败时应分别执行两个阶段。每个 run 目录必须新建，不能覆盖旧记录。

## 分层出口

`preflight.json`、`app-manifest.json`、`launch.json` 和 `run-record.json` 保存在
`<outputRoot>/<run-id>/`：

| Layer | 内容 | 失败时后续状态 |
| --- | --- | --- |
| `machine` | topology 节点、C++ binary 存在性、`readelf` RUNPATH、`ldd -r` closure | `candidate/model/bundle/minindn/workload` 为 `NOT_EVALUATED` |
| `candidate` | profile、runner、build receipt、native binary 和 topology hash | 不允许继续准备 |
| `model` | stage role 顺序、ONNX 文件 hash、tokenizer hash、字节数 | 不允许生成 launch |
| `bundle` | app manifest 和精确 child command；candidate digest 绑定所有输入 | 修改输入后必须重新 prepare |
| `minindn` | 真实 NFD/NLSR、Controller、Authority、Provider 启动和清理 | 启动失败不计为模型或协议结果 |
| `workload` | C++ requester 的 ACK/Selection/Response、多轮会话和输出校验 | 只有 child run-record 为 PASS 才能 PASS |
| `cleanup` | 子 runner 的 MiniNDN teardown 及按 run identity 的进程盘点 | 盘点失败或有残留进程时不得 PASS |

`check` 和 `prepare` 不启动 MiniNDN。`run` 使用 `launch.json` 中保存的完整命令，
并重新核对 candidate digest；profile、模型或二进制变化会 fail closed。失败会留下
`run-record.json` 和 `logs/runner.log`，而未执行的层保持 `NOT_EVALUATED`。

该流程的 native 行为仍由 C++ requester/provider/authority 和 ONNX Runtime 负责；
Python 只负责 profile、文件身份、拓扑生命周期和子进程编排。
