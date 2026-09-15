# TigerCluster Experiments

Tiger分布式推理、SIF构建及Slurm配置的统一入口。当前生产/合并审查不因目录迁移暂停；本目录不声称现有候选满足当前源码资格。

## Layout And Ownership

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

后续分层交付方向已接受：稳定基础 SIF 与外置 DI/UAV 应用包分别构建，以固定组合
验收和运行。当前启动器迁移仍 PLANNED，边界与接续责任见
[SIF build](docs/sif-build.md#current-delivery-direction)。

从仓库根运行，参数沿用原脚本契约：

```bash
bash Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-local-sif.sh --help
bash Experiments/TigerCluster/jobs/spec180/submit.sh <gate> <profile.json> <run-record.json>
```

构建与新镜像输出见 [SIF build](docs/sif-build.md)。
三库固定与接收操作见 [source handoff](docs/source-handoff.md)；共享维护的技能见根目录 [skills/](../../skills/README.md)。
模型准备与运行证据见 [Qwen models](docs/itiger-qwen-models.md)、[Qwen evidence](docs/itiger-qwen-evidence.md)。
Spec180既定用例见 [quickstart](../../specs/180-ack-driven-cross-model-qualification/quickstart.md)；其历史阶段/状态以相应Spec为准，不作为启动旧资格流程的指令。

Tiger 节点的版本边界固定为：登录节点 `/usr/bin/apptainer` 1.3.4 只用于
SSH、Slurm 和资源/路径元数据；分配到的计算节点统一使用
`/home/tma1/.local/bin/apptainer-1.5.3` 1.5.3 完成 SIF 构建、inspect、preflight
和执行。当前候选不得在登录节点构建 SIF，也不得把 1.3.4 当作运行时回退。

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
