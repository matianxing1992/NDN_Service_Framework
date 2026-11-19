# TigerCluster experiments

本目录保存随 Git 交付的 Tiger 操作指南、维护脚本和实验入口。
从当前源码与候选身份开始；历史结果的适用范围见证据汇总。

## 操作入口

| 任务 | 唯一说明 |
| --- | --- |
| 资源选择、模型准备/复用、存储、批量实验、SSH和收尾 | [推荐实验路径](docs/portable-deployment.md) |
| base复用、容器内构建、最终SIF检查与失败重试 | [SIF构建](docs/sif-build.md) |
| 已验证结果、资源基线和反复失败的教训 | [经验与证据](docs/lessons-and-evidence.md) |
| 共享机制修改后的本地前置验证 | [MiniNDN CPU验证](../guides/local-validation.md) |
| 安装、增量编译与实际依赖闭包 | [安装](../guides/installation.md)、[本地开发](../guides/local-development.md) |

日常入口是安装后的 `ndnsf-di cluster check/prepare/up/status/down`。
模型和已支持配置变化复用同一合格SIF；容器执行闭包变化才重建。
容器运行与诊断使用原生C++；Python仅做宿主编排和离线准备。

每次申请allocation前查全群资源、预约与自己的在途作业，选择任意合格节点。
登录节点只做轻量操作；构建、模型处理、完整大文件校验和推理通过Slurm。
等待时按授权窗口通常每600秒复查；不重复提交同一实验。
本页提供命令入口，不授权启动构建、轮询或实验。

## 文件归属

| 路径 | 用途 |
| --- | --- |
| `adapters/slurm-apptainer/scripts/` | 维护的构建、交付、资源与运行脚本 |
| `adapters/slurm-apptainer/profiles/`、`templates/` | 设备配置和Slurm/NFD/进程模板 |
| `jobs/` | 各历史工作流；只按当前任务契约选用 |
| `docs/` | 本页列出的操作手册；不存模型、私钥或大日志 |
| `images/`、`results/` | 候选与运行产物，保持Git忽略 |
| `lib`、`bin` | 共享packaging实现的兼容链接，不复制实现 |

源码交接复用
[prepare-development-handoff.py](adapters/slurm-apptainer/scripts/prepare-development-handoff.py)
和[依赖锁](development-handoff.lock.json)。
封存未提交差异、真实依赖和source seal；大文件SHA在计算allocation中核验。
只搬运本目录不等于获得完整离线发布包。

既有布局迁移见[migration.json](migration.json)；兼容链接和冻结镜像路径保持其原契约。
历史SIF、活动Spec、论文工程及私有数据保留原位。
本次手册整理不重新授予旧候选资格，也不改写历史实验记录。
