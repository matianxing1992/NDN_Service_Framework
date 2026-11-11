# Portable NDNSF-DI deployment on Tiger

适用：installed host管理、Slurm/Apptainer、镜像复用、GPU准入、存储和失联恢复。
结合 installed 部署及近期多模型 CPU 实验的源码与证据；这是操作经验，不是全部验收通过的声明。
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
先核对 seal/manifest 是否涵盖实际必需闭包，再按声明路径及必需参数验证；不凭假设
增加模块名，也不能因必需依赖未声明就忽略它。对脚本参数错误、错误路径或调度失败
记录首边界并修脚本；源和运行闭包未变时复用已建 SIF，重新执行受影响的准入。
若 SIF 交付 Python 扩展，还须在该镜像或 ABI 相同的 builder 中检查 import、
SOABI、RPATH 和动态链接闭包；若交付 MiniNDN runner，检查真实入口的 `--help`。
这些检查不替代原生推理验收。

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

## 每次提交前查询全群资源：CPU和GPU作业统一适用

申请新的Slurm allocation前必须重新检查，覆盖推理、模型准备、构建、诊断、独立oracle
及顺序脚本的每个后续提交；不能只在实验开始时查一次。allocation内已有角色的srun step
复用该allocation的预算，不必逐个重复全群检查。

- 用轻量查询取得全群快照：`sinfo -N`、`scontrol show node`、`scontrol show reservation`，
  以及`squeue`中的运行/排队作业和自己的在途job。记录查询时间；汇总各节点GPU型号/数量、
  未分配GPU、空闲CPU核、内存调度余量、状态和预约。整节点预约或故障节点不能算可用。
  内存调度余量按`max(0, RealMemory-AllocMem-MemSpecLimit)`计算并标明单位，
  它不是Linux MemAvailable；GPU未分配数量也不是设备实际空闲显存。
- 按本阶段的实际需求匹配CPU、内存、GPU和已验证存储；CPU模型准备/推理不因GPU繁忙而停止。
  不沿用GPU部署的多余CPU/GPU请求，也不凭空降低必要内存或超时来制造可调度/通过。
  请求按角色峰值逐项估算，并记录估算依据：Qwen3-4B 两个 CPU Provider 时每个 Provider
  约 8GB（4.4GB 权重半区加工作集），加 Controller、Authority、Requester、Repo 合计约
  32GB 足够；沿用旧的 64GB 请求只会缩小 backfill 可插入窗口，导致长期排队。
  排不上时先按需求把内存和 walltime 降到覆盖真实峰值的水平再投，其次才考虑换节点；
  下调去掉的是余量，不是必要预算，也不能低于已核实的峰值。
  优先当前合格节点；都繁忙时比较当前作业预计释放时间及队列，保留一个有效排队作业。
  `squeue --start`与`sbatch --test-only`仅为预测；后者必须携带实际CPU/内存/GRES/时长等参数，
  不能拿默认1CPU预测比较真实大作业，也不能把新测试作业的预测当成已有排队作业的保证。
- 入口支持候选池时交由Slurm选择；入口要求具体host时，按当前全群观察生成配置，
  不默认绑定示例或历史成功服务器。排队期间每600s复查全群及自己的queue。
  其他合格节点先可用时，仅对尚未执行的作业经维护CLI关闭、确认终态/零残留后改投；
  已运行的作业继续，不因预测变化重启，不并发提交重复实验，不杀他人任务或绕过预约。
  换节点更新host/IP、GPU映射和配置/运行身份，核对实际挂载与节点本地缓存；复用SIF、
  持久Controller材料和已提交Repo模型，不因换服务器重建镜像或重新导出、发布模型。

## 模型复用与批次测量

- 分清四层状态：原始 graph/weights、Provider 已组装模型、ORT Session、会话 KV。
  文件缓存命中只免去相应传输/准备，不保证免去散列校验、分片装配、Session 创建、
  runtime warmup 或当前请求授权。记录每层实际命中；不要把首次进程启动和后续无 KV
  的新会话合并为同一种 Cold，也不要把重启后的文件命中叫 Warm。
- 优先复用已合格 SIF、host control 环境、Controller 材料及完整源缓存。
  仅模型/输入/生成长度变化不重建 SIF。显式 source-cache 实验保留原生规划、
  授权与 activation 的 NDN 路径，但不能据此声称 Repo 网络模型分发通过。
  原文件用已核验的只读共享路径；需要写入的对象记录仅挂载 objects 子目录为 RW，
  不把含只读模型子路径的整个父目录同时挂 RW，避免 CONTAINER_WRITABLE_INPUT_OVERLAP。
- 更换模型或量化版本时，核对 catalog、manifest、source/initializer digest、profile
  与 Authority 的 publication_sources/allowed_model_manifests 一致。旧模型授权不能
  自动覆盖新模型；DI_PROTECTED_GRANT_REJECTED 先检查此接线，不能扩大授权绕过。
- 准备检查器报未知算子时先区分通用 ONNX checker 与实际 ORT schema/kernel 支持。
  保留原失败；小的实际原生加载/执行及独立参考可验证兼容，不能只凭包名判定不支持。
  模型分块权重大小不是进程内存上界。近期 27B INT4 单机双 CPU Provider 在总计
  10 CPU/40 GiB 下完成四组Cold/Warm；两 Provider step 的独立峰值约22.24/17.51 GiB，包含装配与
  执行，不能当作同时峰值或私有稳态堆，更不能据此承诺每 Provider 15 GB 足够。
  此数据只是该32输出、Warm起始context81配置的资源依据，长输出/新线程配置须另核实。
- 用户要求一批实验时，优先一次 Slurm allocation 内顺序消费冻结配置、复用维护的
  allocation/角色 owner，不在循环中反复 sbatch，不复制推理/授权状态机。
  原生 Requester 已有 turns 和独立两轮 conversation groups；先核对当前源码及交付
  是否支持目标配置：Requester 当前最多五组，但独立 ONNX 参考最多八轮/四组，
  整条链采用更严格的上限，不能把五组直接交给八轮参考。
  不要假定提交器已支持多模型批次。当前 deployment oracle 还要求原 Slurm 主作业
  已真实 COMPLETED；不能在活跃 allocation 中伪造 operation/scheduler 终态。
  无需扩展框架时，优先一次部署四组/八请求，再集中独立验证。
  冻结配置/只读输入与输出目录分开；output 的父目录会整体挂 RW，不能包含冻结配置。
  应用 generationId 是32个小写十六进制字符（16字节），不是64字符的完整 SHA256。
  各组结果立即持久化；清理成功才切换模型。普通样本失败可保留后继续，进程残留、
  OOM 或共享服务异常先收尾，不能污染下一组。每批预留 oracle、归档与退出时间。
- 对比生成长度时使用独立会话，从相同 Cold 输入/输出建立相同 Warm 起始 context。
  例如 Cold 输入25/输出32，再追加24，Warm 起始81；从实际 token/checkpoint 核对，
  不能只检查配置。32/128/256 不能在同一会话依次追加后直接比较。
- 分开报告 allocation、启动/源准备、Cold、Warm、独立参考与清理时间。
  ORT Session 创建、图优化、runtime warmup 和业务 prefill/decode/finalize 分别列出。
  “首个业务 ORT Run 前”可能包含 warmup ONNX，不能一律称非 ONNX；业务 ORT 总和
  不是全生命周期 ONNX 总和。使用互斥 wall 分区，嵌套 CPU/机制计时另列。
  每个模型固定实际量化、算子、切点、线程与 SIF 身份；不同量化的速度差不能单独
  归因于模型参数量。以独立会话为重复单位，保留失败样本及可重算统计。
  Slurm/actor CPU数量是调度预算，不能直接写成ORT线程数；核对实际runner的intra-op设置。

## GPU资源可见、可准入、实际执行分别取证
- Slurm物理GPU标识、`SLURM_STEP_GPUS`、step的`CUDA_VISIBLE_DEVICES`、
  容器内CUDA逻辑ordinal、UUID和配置设备号是不同层。
  使用维护路径的`allocated:N`等受支持映射，保留实际step环境及UUID；
  不能在重映射后仍把宿主ordinal直接交给ORT。
- 运行`--nv`、GPU枚举或Provider READY只证明对应边界。
  native签名offer必须提供该设备的新鲜资源观察，不能仅靠硬编码“足够显存”。
  CUDA ordinal到UUID、NVML显存和offer准入的生产逻辑在C++，
  不用实验Python遥测替代。
- 对本次 Qwen 27B GPU 流程，先用同一候选 SIF 和部署路径完成 0.6B 的 cold/warm、
  独立 oracle 与正常退出检查；共享机制修改先通过本地原生检查和 installed MiniNDN
  CPU cold/warm。小模型通过只证明该条件下的路径，不能代替大模型结果。
- 实际GPU证据绑定request/attempt/plan、provider进程、模型及分配，并核对ORT
  execution provider/profile中的CUDA执行与CPU fallback策略。
  归档profile实际指向的文件；不因JSON里写着CUDA就算GPU执行通过。
- 若要求两节点执行同一请求的依赖任务，核对不同实际主机和依赖/result链。
  一台机器两个GPU Provider可作补充，不能关闭明确的跨节点验收。

## 容量检查包括配额、写入和峰值副本

`df -h`/`df -i`、用户/项目quota与目标文件系统的实际写入能力是不同检查。
按身份和路径查询可用配额；不可查询时明确未知。
区分每个 Provider 的应用缓存上限与文件系统/账号配额：提高配置不会增加系统配额。
`EDQUOT` 及当时的目录占用只能确定写入边界，不能反推具体配额。Memphis 通用 HPC
页面描述的默认值可能针对其他集群，不能未经核实套用于 iTiger 账号。
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
`/scratch/$USER` 需要在实际计算节点内验证：登录节点和计算节点的同名路径可能
对应不同的节点本地盘，且账号可能无权创建该目录。已观察到的 job-private `/tmp`
可位于计算节点 NVMe；每次在实际 allocation 中核对挂载、可写性和容量，不能承诺
跨作业保留。可用现有 Provider `artifact-cache-dir` 将下载/组装缓存放到该盘，
按完整 Provider 身份使用稳定的逻辑路径；持久模型原件、Requester Repo 和必需
密钥保留在已确认可复用的共享存储。预算分别包含每个 Provider 的缓存峰值。

模型准备中断后，完整 root 命中不证明部分对象可恢复。核对内容和 Producer 身份、
原始 Data、Owner 密钥与保护上下文；对比准备前后的对象身份和字节数，区分首次
补齐缺失材料与重复发布。相同明文摘要的受保护对象可能有不同 key、epoch 和引用，
不能只凭摘要相同删除。一次本地 C++/CPU 检查证明了部分对象跨版本复用；具体模型
和目标环境仍需各自的跨作业验证，不能把该检查推导为 27B 已通过。

用户要求将 Controller 的公参/主密钥配对、根证书、对应私钥及必要的撤销和代际状态
保存在 `/project/$USER` 并优先复用。先核对实际实现、身份和原有凭据；密钥目录
0700、私有文件0600，仅向对应角色提供。首次建立、正常重启和合法换代要区分，
损坏或身份冲突不能静默覆盖；持久密钥不代替当前授权/撤销检查。当前 Controller
generation 与签名凭据已有持久路径；最近一次源码审查发现 ABE master/public pair
跨进程恢复尚未实现。后续应复查实际源码及恢复证据，不得假定拷贝证书或目录已完成。
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
共享NFS跨主机的st_dev设备号不稳定；不能当作全局文件身份。保留inode/大小/mtime与
内容SHA校验，删除前再在同一主机检查身份稳定，不忽略实际文件变更。

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
分开报告模型准备、首次请求及 warm 请求；传输大小注明载荷或 wire，速率注明
字节和时间边界。不要把整体耗时减 ONNX 时间称为网络时间，也不要把嵌套耗时相加。
清理原始 profile 前保存必要的分组统计与文件摘要，核对归档完整、作业已停止及
文件身份；按 inode/link 计实际释放量。删除逐事件数据后，明确无法重放时间线或
重新计算未保留的分位数。历史原始 profile 与当前失败调查所需数据分别判断。

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

## 2026-11 Qwen3-4B CPU 缓存、配额与挂载教训

本节来自 Spec222 的 4B CPU + Tiger 实验（r28–r34）。这是操作经验，
不把其中的 job/镜像编号设为默认值；新任务仍以当前源码、配置和证据为准。

### 配额与版本保留

- `df` 的整盘可用空间不是用户/项目 quota。小文件 `scp` 报
  `Disk quota exceeded` 就是配额边界；`df` 仍显示空闲并不能否定它。
- 4B 的 fragment cache（约 12–23GB）与 Provider 的 `material.spool` 各占一份，
  二者在同一 `cache_root` 上叠加。按“最新 cache + 本次 spool + assembled 模型”
  估算峰值，而不是只看模型大小。
- 每种数据只保留最新可用版本：同一用途的最新 SIF、最新 cache、最新 release
  manifest；新候选可用后，旧候选 SIF（约 4.3GB/个）和旧 cache 按显式 allowlist
  回收。保留 build record、`runtime.def`、preflight 和 evidence 作为身份记录。
- 只删 superseded 的 `ndnsf.sif`/旧 cache；不删模型、Repo、当前 assembled cache、
  证据和 build 元数据。回收后做一次有界写入探针；`df` 不变但配额已满时，
  以实际写入失败为准。

### cache_root 必须在容器内可见

- 部署的 `run-container.sh` 把 `$scratch:/scratch:rw` 绑定进容器，宿主 `/scratch`
  在容器内被部署 scratch 覆盖。把 `storage.cache_root` 放到宿主 `/scratch/...`
  会在 Provider 启动的 `IMAGE_ROOT_INSPECTION` 阶段失败：
  `IMAGE_INSPECTION_HOLDER_EXIT:1`（expected exit 2），与模型/推理无关。
- 优先把 `cache_root` 放在站点默认可见的 `/project/...`，或放在显式 bind 的路径下；
  `/project` 的用户配额要同时覆盖 cache 和 spool。
- 换 `cache_root` 等于换运行身份/挂载面：先核对容器内路径可达，再重跑；
  不要把它当成纯配置。

### 用 assembled cache 做快速迭代

- assembled role cache 在
  `<cache_root>/provider-*/stored/<identity>/assembled/<role>/<recipeDigest>/`，
  含 `model.onnx`（+`.data`）、`manifest.json` 和 Provider 签名的
  `manifest.signature`；命中后跳过 material fetch 和角色组装。
- fragment cache 在 `<providerCacheDir>/material/sha256/...`，按 payload digest
  寻址、与角色无关；首次填充后，后续整组命中不再走 NDN。
- 冷组装路径和 assembled-cache 命中路径都必须绑定完整的 runner 元数据。本次 4B
  的 Stage-1 在冷路径缺少 `unrankedInputNames`，warmup 仍用 rank-0 占位符，报
  `Reshape ... shape tensor must be a vector`；assembled-cache 命中路径有绑定，
  因此 32 tokens 成功。改 runner 元数据后要同时验证两条路径。
- 用 assembled cache 复现时，先看 `CACHE_ACQUIRE`、`RUNNER_CREATE_BEGIN` 和
  `EXECUTION_EVIDENCE_UPDATE` 的 `warmupSkippedUnrankedInput`；不要把 warmup
  失败当成真实执行失败。

### 本地 0.6B 与 Tiger 包络

- 本地 0.6B 是逻辑门：覆盖 installed 应用、Repo stored 读取和通用角色装配，
  但不覆盖 4B 源的空 shape 常量、SIF 挂载/quota/55 分钟 request 上限，
  以及两 Provider 的运行时依赖。不要把 0.6B PASS 当成 4B 部署验证。
- 4B 的 shape 元数据要单独做 fixture；容器 mount、`cache_root` 和配额要在本地
  或单机用同一 SIF/config schema 做 preflight，再上 Tiger。

### 提交与 oracle

- 每次提交前查全群和自己的 queue；`PENDING (Priority)` 在节点有 16 个空闲 CPU 时
  也可能排不上。换节点（如 itiger03→itiger02）后确认新 allocation 启动，再取消旧
  作业；不要并发跑两份共享 Repo store 的部署。
- 4B 一 turn 32 tokens 的 inference 成功不等于 oracle PASS。
  `run-27b-cpu-first.sh` 会在 deployment down 后用
  `reference-oracle.sbatch --wait` 提交独立 oracle，oracle 也可能
  `PENDING (Priority)`；owner 的 `oracle: NOT_RUN` /
  `CLI_COMPLETE_ORACLE_PENDING` 是待验证状态，不能写成 PASS。
- 27B：51GB material 的首次 NDN fetch 在 55 分钟窗口内不现实；先做离线/本地
  fragment 填充或更快的本地读取，再谈 27B 推理。存储按 cache+spool+Repo 预算，
  内存按 64G 以上估算，不照搬 4B 的 32G/13 CPU。
