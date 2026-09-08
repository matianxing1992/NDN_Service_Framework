# Research And Existing Implementation Review

## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: plan
- Origin Date: 2026-09-06
- Verification Status: UNVERIFIED
- Version Label: spec183_plan_v1

## Question And Method

在固定 NDNSF-DI/YOLO/依赖/SIF/配置下，能否在两个 Tiger 节点重复完成同一请求的真实分布式推理，并用可复用脚本在昂贵运行前拒绝常见环境错误？这是功能与部署复现研究，不做优越性假设检验。
控制变量为 E candidate、模型/input/oracle、请求数、timeout、角色放置和 GPU class；新 allocation/host/GPU UUID 是环境复现因素。主要结果为数值/安全/完整生命周期通过率；耗时只作描述，不从 6 条 measured 样本推断显著性。

## Evidence And Decisions

2026-09-08 部署决策更新（用户明确要求，工程方案，不新增统计研究）：采用
[基础SIF + 外置app](../../Experiments/TigerCluster/docs/runtime-app-layers.md)。
理由：DI/UAV频繁变化不应迫使基础NDN/NDNSF库重建；209981已证明同一历史SIF
加外置C++二进制可完成两物理节点通信。其证据不涵盖DI ABI或GPU。
旧全量应用SIF路径仍在代码，须迁移；应用自己的.so允许受控部署，临时覆盖
基础库或从活跃宿主venv取依赖仍不接受。回退选择完整合格组合。见T002.layer、
T004.layer、T011.layer与T007.layer；不为文档改动重复任何运行实验。

| Current source / document | Finding | Decision |
| --- | --- | --- |
| `Experiments/TigerCluster/development-handoff.lock.json` | 固定四库/5 wheels/base SIF；完整新组合未验收 | 沿用锁，不把最新 branch HEAD 隐式当运行源码；有必要修复才登记新 lock |
| `runtime/baseline.py::load_profile`, `container_command`, `collect` | schema 固定 nodes=2/CPU，容器 cwd/身份/env 隔离已有，collector 有反假绿测试 | 复用底层原语，新 YOLO consumer/profile，不放宽旧 CPU 合同 |
| `jobs/baseline/submit.py` | 只有 baseline/service-echo，单一 prepare/submit/local；不是 YOLO 入口 | 新建薄 `jobs/yolo/submit.py`，不要照抄整个 launcher |
| `jobs/spec180/yolo-functional.sbatch` | nodes=1、1 GPU；wrapper 到 `supervise-tiger.py` | 仅参考单节点闭包，不机械改为两节点 |
| `jobs/spec180/supervise-tiger.py` | 四角色 BackboneNeck/DetectShard0/DetectShard1/Merge | 沿用 DAG/角色/原生执行接口，补跨节点 worker substrate |
| `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py` | 输入、lifecycle observer、grant seam、numeric result 已有 | 应用层复用；不将逻辑搬到 scheduler 或等待 Spec182 |
| `specs/180-ack-driven-cross-model-qualification/contracts/yolo-numerical-evidence-v1.md` | `[1,N,6]`、class/shape、abs+rel tolerance 已定义 | 原样注册：atol=1e-3、rtol=1e-4、confidence>=0.001；不看结果调容差 |
| `docs/two-node-baseline.md` | B003 尚未完成，Local R8 FAIL，当前 58 项为源码单测 | 保留历史证据，新的 source/SIF 从实际路径重新验证，不要求回头重跑旧镜像 |
| `docs/failure-log.md` | 同名 ABI 库、cwd、入口/module、Collector 假绿、权限路径均曾失败 | 明确转为本 Spec 配置/集成/闭包负例 |

以上 Tiger 缩写路径相对 `Experiments/TigerCluster`。CodeGraph query 已使用，并以 exact source 校验脚本行为；第一次 `--limit` 参数不支持，已改合法调用。分支切换后 index sync 已完成（350 changed files），未以旧 index 结果作为新代码行为证据。

## Rejected Alternatives

- 为每个失败再造一个 wrapper/环境配置：增加重复权威；一个 profile、shared runtime 加 YOLO consumer 已足够。
- 直接用已有 r119 SIF：该文件是依赖 base，不能证明新源码/新 ABI。
- 先做完整 Qwen/GPU/速度矩阵：不能更快证实当前 YOLO 环境，超出本轮目的。
- 调 timeout 或用 CPU fallback 得到输出：改变比较身份，可能掩盖配置/通信错误。

## Remaining Operational Inputs

T001 必须登记目标 GPU/partition/account 配额、实际 Apptainer、base/model/oracle/密钥材料可访问性和容量估算。当前未运行 SSH/Slurm/构建，不宣称它们存在或已合格。这些是可执行的接收任务，不是未决定的架构；有缺失时保留 WAITING_EXTERNAL_INPUT，禁止临时编造 hash 或 PASS。

## Analysis And Repetition

先单节点 1 warmup+1 measured；两正常 allocation 各 1 warmup+3 measured。仅同一个冻结 fixture，验证工程复现，不验证泛化识别精度。每条记录从 requester 发布到解密/组装/数值检查完成的 monotonic latency，warmup 单列；阶段用各进程 monotonic duration，不减不同主机 wall clock。
结果逐 run 列 attempted/completed/numericalMatched、错误、maxAbsError、延迟及后端；可列平均/中位数，6 条请求不报告有说服力的 P95 或 CI。需要正式性能结论时另注册至少 60 秒窗口和足够重复的设计。
