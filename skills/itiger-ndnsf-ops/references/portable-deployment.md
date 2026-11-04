# Portable NDNSF-DI deployment on Tiger

适用：installed host管理、Slurm/Apptainer、镜像复用、GPU准入、存储和失联恢复。
依据2026-10-27源码与Spec217证据；这是操作经验，不是全部验收通过的声明。
新任务先核对当前源码和选定要求，不复用旧job、主机或镜像号作为当前事实。

## 选入口并定位变更边界

- 配置解析：`ops/deployment_config.py`；本机管理：`ops/cluster.py`；
  Slurm提交/状态/恢复：`ops/slurm.py`；容器角色及挂载：`ops/container.py`；
  节点角色/收尾：`ops/allocation.py`。均在
  `NDNSF-DistributedInference/ndnsf_distributed_inference/` 下。
- 使用已安装的 `ndnsf-di cluster check/up/status/down --config PATH`；
  核对当前help和schema后使用。普通安装按wheel声明解析依赖；
  `--no-deps` 的测试夹具不能证明普通安装闭包。复用Waf安装及已有Slurm适配器，
  不另写生产提交器、SSH编排器或Python推理状态机。
- host平台目前要求一个 `host: local` 节点。两台任意物理机不是已实现的host启动模式；
  MiniNDN、本机双Provider和Slurm多节点结果分别报告。
- 模型、输入、token数、Provider数量和放置属于配置，前提是当前schema和运行时支持。
  配置变化生成新运行身份，不因参数变化重建通用镜像。
- C++、容器内绑定、依赖ABI或镜像内实际执行代码变化，回到对应构建/ABI检查。
  host-only控制修复可更新独立host wheel；核对同名模块究竟从host还是SIF加载，
  不把“Python文件修改”一概当成host-only。
- 外部依赖/SDK的base与NDNSF源码层分开。保持选定Apptainer版本及实际binary/hash；
  构建地点由当前方案和可验证能力决定。计算节点构建用有界CPU allocation，
  不把历史Spec的“只能本地构建”强加给另一个已授权方案。

## 一份身份贯穿准备、绑定和运行

Tiger 登录节点是共享入口，只做轻量编辑、作业准备/提交和有界状态查询。
模型训练、导出/量化、CPU/GPU 推理、SIF/依赖编译构建，以及大目录递归扫描、
几十 GiB 文件的完整摘要校验或批量数据处理，都放入 Slurm 计算节点 allocation。
交互式任务也先获得 allocation；使用 CPU 资源的准备工作不必申请 GPU。
减少线程数、使用容器或 nohup 不会把登录节点变成计算节点。
轻量发现保留 df、配额、挂载和调度信息；不在登录节点轮询中递归 du 模型目录。

保留source seal、依赖/SDK锁、构建记录、SIF SHA、native/扩展清单、host wheel、
release manifest、模型/tokenizer、配置和oracle身份。Git HEAD不能覆盖未提交字节。

准备产物通过不等于绑定通过；绑定产物已写出也不等于调用端验证通过。
运行前要求本轮绑定成功、owner清理完整、相同准备记录/主机、绑定原始输出摘要，
以及绑定SIF与已验ABI镜像相同。不能在绑定的镜像比对失败后直接运行已生成配置。
新鲜授权/保留期限在准备实际提交时绑定；排队或资源等待后重查有效期，
不先冻结短期凭据再长时间等待。
运行前核对requester catalog与authority `publication_sources`的内容、源和profile身份。
ONNX外部权重还须将`source.initializer_digest`绑定到授权策略的
`initializer_object_digest`；原生授权接口支持该字段，遗漏会在准备后拒绝manifest。
配置一致性检查不代替运行中的原生授权，不扩大allowlist或放松比较来绕过拒绝。

host上的srun负责进入各节点的精确SIF；容器内运行NFD/nfdc和native角色，
不依赖容器内嵌套Slurm或宿主推理库。每角色独立HOME/PIB/TPM和控制器证书，
以真实认证/服务就绪决定启动顺序，不以PID存在代替。
跨节点路由覆盖实际请求/控制和Producer命名空间，按配置核对双向连接；
节点地址从当前配置或DNS得到，不能由rank或主机后缀推断。

## GPU资源可见、可准入、实际执行分别取证

- 查询全部候选节点的CPU、内存、GRES类型及已分配量，并检查自己的在途job。
  硬件目录或旧节点快照不证明当前空闲。使用明确且足够的CPU/内存/GPU/walltime预算；
  资源不足时继续扫描全部符合约束的节点，或保留唯一有效排队作业。
  当前会话用户要求等待600s后重试；单节点繁忙或一轮无空闲资源不是直接停止任务的理由。
  每次等待后重查全群及自己的queue，不自行杀他人的任务或无故重建镜像。
- Slurm物理GPU标识、`SLURM_STEP_GPUS`、step的`CUDA_VISIBLE_DEVICES`、
  容器内CUDA逻辑ordinal、UUID和配置设备号是不同层。
  使用维护路径的`allocated:N`等受支持映射，保留实际step环境及UUID；
  不能在重映射后仍把宿主ordinal直接交给ORT。
- 运行`--nv`、GPU枚举或Provider READY只证明对应边界。
  native签名offer必须提供该设备的新鲜资源观察，不能仅靠硬编码“足够显存”。
  CUDA ordinal到UUID、NVML显存和offer准入的生产逻辑在C++，
  不用实验Python遥测替代。
- 实际GPU证据绑定request/attempt/plan、provider进程、模型及分配，并核对ORT
  execution provider/profile中的CUDA执行与CPU fallback策略。
  归档profile实际指向的文件；不因JSON里写着CUDA就算GPU执行通过。
- 若要求两节点执行同一请求的依赖任务，核对不同实际主机和依赖/result链。
  一台机器两个GPU Provider可作补充，不能关闭明确的跨节点验收。

## 容量检查包括配额、写入和峰值副本

`df -h`/`df -i`、用户/项目quota与目标文件系统的实际写入能力是不同检查。
按身份和路径查询可用配额；不可查询时明确未知。
[iTiger官方Wiki](https://github.com/itiger-cluster/itiger-cluster.github.io/wiki/Running-A-Model)
写明HOME默认20GB并建议大环境使用`/project`；该页最后编辑于2024年，
不能替代当前账号配额查询，更不能把`df`显示的整盘容量称为个人额度。
必要时在目标目录做有界写入、flush/fsync并清理的探针，避免仅靠空目录或touch。
预留模型/Repo/cache/生成副本、日志和终态receipt的峰值空间；
构建目录的写入探针通过不能证明推理Repo目录有余量。
登录节点scratch不自动在计算节点可见；核对共享性和保留期限。
分别检查跨作业保留、跨节点共享、实际挂载和清理政策，不从目录名推断。
Slurm job-private /tmp 可随 allocation 回收；/var/tmp 不能一概视为随作业删除，
同节点两次作业间文件仍在也不证明永久保留。可复用模型、Repo 和缓存放入已确认
保留策略/容量的稳定目录；临时盘只承接可丢弃的 staging 和中间副本。
内容、Producer 和保护上下文匹配时复用原生提交记录，不因换作业就重新导出或发布。
构建中间文件、可重建传输副本使用实际allocation内的节点本地临时盘；
核对`TMPDIR`、`SLURM_TMPDIR`和mount，不假定变量存在或跨节点共享。
在作业内检查可用空间；空闲字节不是个人预约额度。作业结束前归档必要证据，
不把`/tmp`当作可复用模型/Repo的唯一副本或永久证据库；保留配置、凭据和持久结果。

`EDQUOT`导致结果/日志缺失时，记录最先捕获的配额边界。
这不证明内部推理只因配额失败，也不能推算缺失的ONNX时间。
没有测量的数据标记不可用，不把wall-minus-CPU称为网络时间。

可回收候选先按归属、关闭状态和可重建性分类：
旧构建对象、已归档且关闭的Repo副本、废弃源码传输包、验证过的镜像重复副本。
保留当前模型/内容身份缓存、有效运行目录、原始失败调查所需子集及持久证据。
同用途复用有锁的磁盘workspace，不在每次重试创建模型大小的目录，不占用/dev/shm。

删除按确切路径allowlist执行，检查引用进程、mount、symlink、owner和锁。
镜像仅在需要保留的副本完整SHA核验及保留策略满足后删除冗余副本；
旧镜像不因“旧”自动可删。记录实际释放字节。
生成精确inode/mtime/ns等删除身份时，不经JavaScript Number往返超过安全整数的值；
使用Python整数或字符串保留精度。

## 执行、调度终态和host清理不是同一状态

有限命令必须有owner、deadline、信号处理、日志和回收记录。
排队作业可按授权等待；提交响应不明先查已记录的job/命名空间，不能重复提交。
Response、成功marker、scheduler COMPLETED或leader退出都不证明进程组已清理。

缺allocation/final audit时，已结束的Slurm job仍可能留下UNKNOWN。
使用公共down/recovery路径，绑定原job、原node/boot、config/release、进程身份和
本次auditor身份，检查UID/job cgroup及相关宿主进程。恢复audit拿不到原节点资源时，
保持`needs_closeout`，不伪造空allocation或清理PASS。
恢复成功只证明相应清理；失败的执行仍是FAILED，oracle不能因此变PASS。

原运行audit需要原节点，但其他可用节点可能仍能承载独立的新运行。
按当前授权、隔离条件、quota和自己的queue决定能否继续；
不能因一个节点繁忙就假设全群不可用，也不能用新节点audit关闭旧节点的未知状态。
同一deployment的旧操作仍需closeout时，不覆盖配置或绕过up的拒绝。
独立新运行使用不同deployment身份及隔离的operation/evidence和可写存储路径；
先核对新配置没有复用旧运行仍在保护中的资源。
保留旧命名空间和数据，直到其收尾证据及回收条件成立。

## SSH按实际停止阶段诊断

收到集群方 SSH 故障通知时，在现有运行记录保存原文或摘要、来源和接收日期；
只有用户转贴时明确注明，不能补造公告链接或把接收日期当发布时间。
通知是服务方已知访问异常的证据，不证明全部节点宕机、账号被封、作业终止，
也不能永久写成“Tiger 无法连接”。保留已提交 job ID、模型和已有镜像；
连接恢复后先查原作业状态、退出码、日志及实际产物，再决定重试哪一步。
作业若通过 SSH 回传结果，还需确认归档完整；调度器 COMPLETED 本身不证明回传成功。

| 观测 | 能确定的边界 | 不能据此断言 |
| --- | --- | --- |
| VPN显示Connected | 客户端报告隧道已连接 | Tiger路径、SSH及账号正常 |
| TCP22连接超时 | 尚未建立SSH连接 | 账号被封，或一定是服务器宕机 |
| 握手和已知host key匹配，发送公钥后超时 | 当前已到认证等待 | 已接受密钥，或已证明账号锁定 |
| Permission denied/认证拒绝 | 本次认证未通过 | 一定是封号；还可能是密钥/策略问题 |
| 认证成功但远端命令无结果 | 已过认证 | 应用、文件系统或调度器可用 |

只核对必要的VPN状态、当前route/DNS和有限SSH输出，不打印凭据。
另一个校园SSH端点可达只说明部分路径可用，不能替代Tiger或证明同一账号/存储可访问。
未经授权不更换密钥、清known_hosts、登录另一个集群或联系管理员。

轮询按用户指定间隔和窗口执行；本次会话约定600s时遵守600s，
不把它推广为所有任务的硬编码值。一个owner有界循环，保留每次结果，
不要同时启动多个重试器。观察超时不等于进程结束；核对原handle/PID身份后继续。
用户重连VPN是可立即复查的新事件。成功或达到窗口上限后正确结束，
循环退出码0本身不证明SSH成功；检查实际状态/命令结果。
SSH恢复但资源不足时继续资源轮询，不能把登录恢复当成实验完成。
单个有界轮询窗口结束时核对进程及最后结果；已授权任务仍需等待时，从检查点续开
下一有界窗口，避免重复owner或静默停止。遵守用户停止、任务预算及外部访问限制。
文档/skill更新本身不授权启动轮询或集群任务。

## 收尾和证据强度

按当前验收要求保存首轮、warm及失败请求的逐请求时间、CPU、计数、测量边界和来源；
加和wall分区与嵌套测量分开。普通性能运行关闭诊断/per-token日志，
保留必要结构化测量。归档可重算数据、相关日志、首失败、命令和身份，
验证可读后才按allowlist清理可重建产物，不搬迁整个临时树来冒充归档。

分别报告安装闭包、build/ABI、local inference、cluster CPU、GPU执行、
两节点依赖执行、oracle与host清理。quiet-execution oracle未评估的
grant/material/placement/restore等项目仍是NOT_EVALUATED；
不能由窄oracle PASS推导全路径安全或全部Spec完成。

源依据：`ops/`上述模块、`examples/Spec189TwoProviderOracle.cpp`、
`examples/DI_NativeProviderExecutable.cpp`、
`cpp/ndnsf-di/NativeProviderOfferV3.cpp`、
`cpp/adapters/onnx/cuda-device-memory.hpp`（后二者在DI目录），以及当前
`specs/217-portable-di-deployment/evidence/b217-11.md`及相关原始receipt。
证据目录是本地历史输入，fresh checkout可能没有；缺失时重新核对相关实际边界，
不把此参考作为运行或通过证明。
