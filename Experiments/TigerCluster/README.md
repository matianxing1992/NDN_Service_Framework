# TigerCluster Experiments

Tiger分布式推理、SIF构建及Slurm配置的统一入口。当前生产/合并审查不因目录迁移暂停；本目录不声称现有候选满足当前源码资格。

当前实验机计划为 [Spec183](../../specs/183-tiger-yolo-reusable-experiments/spec.md)：
在 `TigerClusterExperiments` 上固定可复用 YOLO profile/launcher，完成本地验证、
单节点 GPU、两节点分布式推理及独立 allocation 复跑。
详见 [tasks](../../specs/183-tiger-yolo-reusable-experiments/tasks.md)。
状态为 PLANNED / NOT_RUN；`jobs/yolo/submit.py` 和 `profiles/yolo-two-node.json`
是计划交付文件，尚不能运行。当前 rank→collector handoff 已有 fail-closed
组件接线，但没有替代真实 worker/SIF/Tiger 资格。新 Tiger 专用脚本、配置、schema、测试工具和操作说明
都放本目录，复用现有 runtime；通用 Core/DI/Repo 源码仍归原 owner。

## Layout And Ownership

实验机当前工作分支为 `TigerClusterExperiments`，从 `Experimental` 的交付提交
`81e251a4` 创建。旧 `UAV-Experimental` 已合并并删除。后续开发机更新通过正常
merge 整合；具体规则见 [分支策略](../../docs/DEVELOPMENT_BRANCHING_POLICY.md)。
源码接收仅为 SOURCE_READY，后续构建与测试仍按交付锁文件执行，最多 `-j4`；同一构建树不并行。

| Path | Owner / purpose |
| --- | --- |
| adapters/slurm-apptainer/scripts/ | Tiger构建、暂存、资源检查和运行脚本 |
| adapters/slurm-apptainer/profiles/ | RTX5000、RTX6000、H100配置 |
| adapters/slurm-apptainer/templates/ | Slurm、NFD、进程/路由模板 |
| jobs/spec180/ | YOLO/Qwen作业、profile、提交器、监督与结果收集 |
| jobs/spec175/ | 既有streamed-generation作业及冻结profile |
| docs/ | 构建说明和Qwen操作文档 |
| images/ | 新生成SIF的本地存放入口；历史镜像只提供链接 |
| results/ | 新运行输出；不入Git |
| lib / bin | 指向packaging的共享校验库和CLI，不复制实现 |

精确64文件迁移清单、输入SHA-256、执行权限与共享归属见 [migration.json](migration.json)。
源文件内容不变；沿用原目录深度，使脚本的仓库根推导保持成立。
通用OCI/容器、Docker Compose、根目录scripts中的资格/候选工具和生产代码保留其现有owner。

## Entry Points

从仓库根运行，参数沿用原脚本契约：

```bash
bash Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-local-sif.sh --help
bash Experiments/TigerCluster/jobs/spec180/submit.sh <gate> <profile.json> <run-record.json>
```

构建与新镜像输出见 [SIF build](docs/sif-build.md)。

Spec183 的 `tools/spec183_minindn.py` 输入包括 `--run-id`、`--output`、
`--profile` 和必填的 `--preparation-sha256`。后者必须来自该次 issuer
保留的 preparation 摘要，不能临时给任意输入计算摘要作为批准。
包装器验证准备材料、模型清单和实际每 run 密钥后，独占创建
`<output>/<run-id>/host-minindn/`；重用同一输出会拒绝启动。
包装器在本机通过 systemd transient service 执行（需要 system manager、
cgroup 和 root 或 `sudo -n`）。运行时限取已验证 profile 的 staging + startup
+ 一次 request deadline，退出清理使用 cleanupSeconds；不自动重试任务。
`host-minindn/supervisor/` 保留 unit 身份、日志、结果及 cgroup 空置观测。
当前仍只接 Y-B，三场景、网络资源清理证据和语义 host manifest 未完成，
`T010_DONE` 的 `NOT_EVALUATED` 不能放行 SIF 构建。正式执行遵循
T007 → T008 → T009 → T010；当前证据见
[输入绑定修复](../../specs/183-tiger-yolo-reusable-experiments/evidence/t010-input-binding.md)。

三库固定与接收操作见 [source handoff](docs/source-handoff.md)；共享维护的技能见根目录 [skills/](../../skills/README.md)。
模型准备与运行证据见 [Qwen models](docs/itiger-qwen-models.md)、[Qwen evidence](docs/itiger-qwen-evidence.md)。
Spec180既定用例见 [quickstart](../../specs/180-ack-driven-cross-model-qualification/quickstart.md)；其历史阶段/状态以相应Spec为准，不作为启动旧资格流程的指令。

## Compatibility And Review Boundary

开发机负责代码、静态审查与测试，实验机负责SIF构建和Tiger集群运行；MiniNDN依当前Spec安排。
另一台机器应从明确的完整源码基线开始，按本README及构建说明操作。
新运行显式使用`results/<run-id>/`，新镜像使用`images/<candidate>/`；
本地历史镜像链接不随Git交付，须另行获取或构建候选并核对身份。

旧packaging中的adapter/jobs路径及两份文档保留相对symlink，指向这里的唯一实体。
profile、source seal、测试和冻结脚本的旧路径因此仍可解析，哈希不因移动而被重写。
共享lib/bin反向链接回原owner；无链接指向合并审查工作树。

本轮只改变宿主源码布局。历史SIF、source seal、run record和证据保留原位；新路径不赋予镜像新的资格。
源打包器继续按冻结清单物化旧镜像内路径；不要仅复制本目录就宣称得到了完整离线发布包。
提交/搬运源码时应包含仓库中的canonical文件与兼容链接，并检查共享依赖。

合并审查保持在独立工作树。同步该工作树的后续修复时，将旧路径修改应用到本清单对应目标，
重查受影响脚本/测试；不要把兼容链接替换成第二份实现。
兼容链接在所有维护调用方与发布输入清单改为新路径后集中移除；历史证据不回写。
