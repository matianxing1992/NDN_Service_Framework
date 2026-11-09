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

所有后续SIF构建统一使用 **Apptainer 1.5.3**，实验机也须先核对/升级；命令固定传入 `--expected-apptainer 1.5.3`。登录节点的1.3不作为构建目标，完整规则见[SIF build](docs/sif-build.md#apptainer-version-policy)。历史镜像不因版本升级自动获得新资格。

当前分层交付为 [base SIF + NDNSF](docs/two-layer-delivery.md)：base 提供全部外部依赖与 SDK，
NDNSF 层统一承载本仓库模块的选定目标；可从已有 base 补齐依赖，无需从头重建。
当前正常路径为 `build-local-sif.sh` + Waf-installed 完整 SIF，不需要外置 APP。
[SIF + APP delivery](docs/sif-app-delivery.md) 仅供历史兼容，不是新镜像的额外构建步骤。

若容器内原生编译已完成、仅最终打包失败，`build-local-sif.sh --build-only` 可显式传入
`--builder-checkpoint ROOT --builder-checkpoint-retention RECEIPT --builder-checkpoint-sdk-sha256 DIGEST`。
仅接受带原始definition、完成日志及原生/Waf/Python清单的已核对安装树；保留原final阶段，
执行前后校验checkpoint，并记录编译结果复用。该输出仍未合格，须继续准入与真实推理验收。
构建临时目录须在实际磁盘上；目录叫`/var/tmp`并不能证明其文件系统不是内存。

`prepare-development-handoff.py prepare-runtime` 可用 `--source-seal PATH` 复用
已验证的源码 seal 和同目录 `workspace.tar`，不要求计算节点安装 Git。
`--workspace` 的全部选定文件必须与 seal 的路径和内容摘要一致；原归档保持字节不变。
未传该参数时仍从当前 Git 工作树封存。模型/SIF 的完整摘要及 SDK 校验在 Slurm allocation
内执行，登录节点只准备小型控制输入和提交作业。

大型模型的准备预算按材料量和实测发布速度设置，不用固定等待代替就绪。
`ndnsf-di cluster up/prepare --timeout SECONDS` 在 `slurm-apptainer` 下支持最多 86400 秒；
本机/MiniNDN启动和 `status/down` 仍最多 3600 秒。启动总预算须覆盖排队、准备、请求与收尾，
并协调配置中的 requester `limits.bootstrap_ms` 及 Slurm walltime。修改这些配置无需重建 SIF。

`ndnsf-di cluster prepare --config deployment.json --timeout SECONDS` 使用同一原生Requester
的准备调用，排除Provider及GPU预留。`model_input.mode=publish-stored` 还无需推理
Authority、turns 或推理密钥配置，只保留模型策略、Controller/Repo 和私有 journal/handle
路径；默认 source 模式保留原有准备配置兼容性。CLI 在推理字段校验前投影准备配置。
成功状态为`PREPARED`，不是推理完成；随后用原始配置执行`cluster up`，复用相同模型、
owner凭据及持久Repo。准备不包含Provider取回、拼接或ONNX装载。操作模式绑定到冻结配置
摘要；临时目录的保留与跨节点可见性须独立验证。该入口需要包含`--prepare-only`的新版
原生Requester；旧SIF不支持。Spec220的完整运行资格仍以其实际证据为准。

从仓库根运行，参数沿用原脚本契约：

```bash
bash Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-local-sif.sh --help
bash Experiments/TigerCluster/adapters/slurm-apptainer/scripts/build-sif-app.sh --help
bash Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-sif-app.sh --help
bash Experiments/TigerCluster/jobs/spec180/submit.sh <gate> <profile.json> <run-record.json>
```

构建与新镜像输出见 [SIF build](docs/sif-build.md)。
三库固定与接收操作见 [source handoff](docs/source-handoff.md)；共享维护的技能见根目录 [skills/](../../skills/README.md)。
模型准备与运行证据见 [Qwen models](docs/itiger-qwen-models.md)、[Qwen evidence](docs/itiger-qwen-evidence.md)。
Spec180既定用例见 [quickstart](../../specs/180-ack-driven-cross-model-qualification/quickstart.md)；其历史阶段/状态以相应Spec为准，不作为启动旧资格流程的指令。

## Compatibility And Review Boundary

Slurm部署使用隔离环境，不应依赖登录shell中临时追加的Apptainer辅助程序PATH。
自定义用户安装的Apptainer应在其`apptainer.conf`的受支持`binary path`中包含实际
`squashfuse_ll`等辅助程序目录；保留系统默认搜索目录，不修改容器内PATH。
提交GPU作业前，在同样的Slurm隔离环境中验证原生`ndnsf-identity-check --trust-resources`
输出及正常清理。Apptainer的非空警告仍会被严格信任资源检查拒绝，不能以忽略stderr绕过。
大型发布临时文件使用配置的`scratch_root`，按actor/boot隔离；证据目录只保留测量和receipt。
`storage.scratch_root`应使用已验证的allocation本地磁盘，不从持久模型/Repo/cache根目录
派生。容器会把Core大数据spool和session临时文件放到该根下；把它设在`/project`会使临时
副本和持久数据竞争同一配额。提交前在实际SIF中检查`/tmp`及bulk挂载并验证有界写入；
全局`df`余量和小文件成功不证明模型级峰值能写入。模型、原包Repo和可复用缓存继续使用
独立持久路径。`/scratch`是否共享、`/tmp`是否随作业回收须实测，不能仅按目录名判断。

可复用存储与实验编号分离：同一完整Provider NDN Name使用固定的`artifact-cache-dir`；
Requester的源材料/加密仓库与Repo的`repo_root`也保持稳定。按Name的规范TLV编码确定
目录身份，不能使用可能变化的URI拼写、角色编号或run ID。模型、量化、backend及保护上下文
仍由原生缓存的身份与接纳检查区分；目录相同不表示可以跳过验证。每轮状态、日志和发布
临时文件另行隔离。跨计算节点复用须选择共享文件系统；固定的节点本地路径仅支持该节点
上的复用，不保证永久保留。避免同名角色并发写同一存储，除非原生存储明确支持该并发方式。

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
