# TigerCluster：推荐实验路径

这是可随 Git 交付的操作指南；项目规则仍由根 AGENTS.md 定义。
结果与来源见 [经验和证据](lessons-and-evidence.md)，构建见 [SIF](sif-build.md)，
本地前置验证见 [local-validation](../../guides/local-validation.md)。

## 1. 固定一次运行的输入

从任务 checkout 确认源码、工作区差异、依赖锁、SIF/host wheel、模型和 tokenizer、
配置、oracle 的身份。HEAD 不涵盖未提交代码；旧镜像的 PASS 不转移给新源码。
先沿实际入口核对配置消费与产物接纳，随后才决定重建范围：

| 改动 | 最小动作 |
| --- | --- |
| 支持范围内的模型、输入、token 数、Provider 放置 | 更新配置和运行身份，复用合格 SIF |
| 仅宿主 launcher/挂载逻辑 | 更新 host 控制包并验证；镜像内代码不会因此更新 |
| 容器内 C++、SDK/ABI、实际执行闭包 | 重建受影响候选；外部依赖变化才回到 base |
| 编译完成，仅最终 SIF 打包失败 | 核验 builder checkpoint 后复用编译结果，仍需最终镜像验收 |

容器 runtime、诊断和 oracle 使用原生 C++。Python 限宿主编排或离线准备。
仅在确实交付 Python 扩展时检查其容器构建 ABI；它不是默认推理依赖。
通过 CLI help、SDK、build 或 model preparation 不等于推理通过。

## 2. 每次提交前查全群

登录节点只做轻量编辑、传输/提交控制输入和状态查询。模型准备/量化、推理、
SIF/依赖构建、大文件完整 SHA、大目录扫描在 Slurm allocation 内执行；CPU 工作不申请 GPU。

使用当前账号和维护的连接，提交前留存时间及下列轻量快照：

~~~bash
sinfo -N -o '%N %t %c %m %G'
scontrol show node
scontrol show reservation
squeue -u "$USER"
~~~

查看全群 GPU 型号/数量、未分配 GRES、空闲 CPU、内存调度余量、预约及故障状态。
内存余量按 max(0, RealMemory - AllocMem - MemSpecLimit) 估算；它不是 Linux MemAvailable。
Slurm 未分配 GPU 数不是实际剩余显存；获配后还要核对设备 UUID、可用显存和 native offer。

选择任意满足模型、存储、CPU/内存/GPU要求的节点，不固定历史服务器。
优先给 Slurm 合格候选集合；入口要求具体节点时再据快照选择。
资源请求按本阶段实测峰值/模型与工作区估计，不照抄 192 GiB、64 GiB 或每 Provider 15 GB。
CPU 准备和 CPU 推理不因全群 GPU 繁忙而等待。

排队时通常每 600 秒复查；用户指定的窗口/频率优先。
一个 owner 管理一个有效实验作业；提交响应不明先查原 job，不重复提交。
其他节点先可用时，关闭尚未执行的旧操作并确认终态后改投；已运行作业不因预测变化重启。
squeue --start / sbatch --test-only 是预测，后者要带真实资源和时长参数。
不杀他人作业、不绕过预约，也不并发运行两份会写同一 Repo/cache 的部署。

## 3. 分离模型准备和模型使用

默认使用当前安装的维护入口，从仓库外也能解析其 native 程序和库：

~~~bash
ndnsf-di cluster check --config deployment.json
ndnsf-di cluster prepare --config prepare.json --timeout <seconds>
ndnsf-di cluster up --config inference.json --timeout <seconds>
ndnsf-di cluster status --config inference.json
ndnsf-di cluster down --config inference.json
~~~

按当前 CLI help/schema 设置预算；Slurm up/prepare 最高 86400 秒，
本机/MiniNDN及 status/down 最高 3600 秒。总预算协调排队、bootstrap、组装、请求、
oracle 和清理；准备时间按材料量与实测速率估计，用真实就绪/提交回执判定完成。

| model_input.mode | 用途与成功边界 |
| --- | --- |
| publish-stored | 单独 prepare 发布模型；PREPARED 后仍要验证 exact root/object 可被目标 Repo 读回 |
| stored | 使用 private handle、同一持久身份与原始 Repo Data |
| source | 请求中准备源模型；以实际 native 分支和提交回执判断是否完成发布，不能从 journal 字段推断 |
| source-cache | 已支持的显式推理模式，以摘要核验的文件缓存替代模型网络获取；不证明 Repo 分发通过 |

prepare 不运行 Provider 推理。journal ready、文件存在或对象总数，都不能证明
当前 handle 对应的精确版本已在本次 Repo 中提交。存储修改先用生产读 API 做跨实例/重启读回。
model/quantization 改变时同步 catalog、Authority source/initializer/profile 和权限范围；
保留原生拒绝检查，不扩大 allowlist 来掩盖错配。

## 4. 存储与身份复用

| 数据 | 放置与复用原则 |
| --- | --- |
| 模型源、Repo 原包、private handle、Controller/Authority 状态 | 已核实配额和保留策略的持久目录；本用户优先 /project/$USER |
| Provider fragment/assembled cache | 按完整 Provider Name 的规范 TLV 身份使用稳定逻辑路径；模型/recipe/保护上下文由 native cache 再区分 |
| ORT Session、会话 KV | 进程/授权 lease 内复用；文件命中不等于 Session 命中或 Warm |
| spool、builder、传输副本 | allocation 内已验证的磁盘 scratch，按 actor/boot 隔离；关闭前归档必要证据 |
| 每轮日志和结果 | 独立 operation/run/request 身份；不与可复用模型目录混用 |

df 是整盘容量，配置的 cache max_bytes 是应用上限，两者都不是用户 quota。
官方 [Running A Model](https://github.com/itiger-cluster/itiger-cluster.github.io/wiki/Running-A-Model)
的 2024 页面提及 home 20 GB 和 /project 建议；当前额度仍需账号/项目查询。
未查到限额就标未知；一次有界写入只验证该次写入，不证明整个模型峰值能容纳。
没有证据表明 /project 的 200 GB 是所有用户的固定上限。

容量按原件 + Repo 包 + fragment + spool + assembled + 临时图/运行时工作区的同时峰值估计。
来源缓存可在同文件系统使用已核验 hardlink，跨文件系统才做有界复制。
只读输入根与可写 objects/output 分开挂载；不要让 RW 父目录覆盖冻结输入。

实际容器会把部署 scratch 绑定为 /scratch：宿主同名路径可能被遮蔽。
在目标节点和候选 SIF 内检查 mount、可写性与容量；/scratch、/tmp、/var/tmp 的共享、
跨作业保留与清理策略分别核实，不能由目录名推断。
/var/tmp 不应被宣称必定随作业删除；节点本地缓存也不是永久共享仓库。

Controller public parameters、配对 master key、根证书/私钥和必要代际/撤销状态要匹配保存。
目录 0700、私有文件 0600，仅相应角色可读，不进 Git。必须验证完整跨进程恢复；
保存证书不证明 ABE 主密钥/公参已恢复，持久身份也不替代当前授权检查。

## 5. 从小模型到目标模型

共享机制修改先完成 focused native 检查和 installed MiniNDN CPU cold/warm。
GPU 目标先核对同一候选的 native SDK/ELF/设备初始化，再完成 0.6B GPU cold/warm、
独立输出/状态 oracle 和自然退出，之后才上昂贵的 27B GPU。
已有结果只在相应源码/runtime/SIF未变时复用；0.6B不替代4B/27B的图、挂载和内存验证。

先做单节点两个 Provider，减少跨节点网络变量；跨节点是独立验收条件。
GPU不可用时按授权继续CPU路径或保留一个GPU排队作业，不改变模型/精度来伪造匹配对照。
CPU与CUDA优先消费同一可兼容的原始图/权重包；实际算子、dtype或runtime不兼容才另立变体。
设备优化 Session/cache 属于 Provider，本地优化不重新发布源模型。

## 6. 一次 allocation 做多组，证据按请求分开

复用现有 Requester turns/conversation groups 和 allocation owner，顺序运行独立会话，
不在每组循环 sbatch。先核对整个 oracle 链的上限：已验证的27B基线为四组/八请求；
Requester单端更高的上限不意味着独立参考也能接受。
现有独立 oracle 如要求主作业 COMPLETED，应正常结束后再提交，不能伪造 scheduler 终态。

比较32/128/256输出时，每组从相同 cold 建立相同 warm context。
例如 cold 输入25/输出32，再追加24，warm起点81；核对实际token和checkpoint，
不能在同一会话依次追加后当作同context比较。固定模型/量化、线程、拓扑和采样条件；
Slurm CPU配额不等于ORT intra-op线程数。

分别记录 allocation/启动、源准备、Session加载与warmup、cold、warm、
prefill/decode/finalize、独立参考和清理。互斥wall分区可相加，嵌套CPU/机制计时另列。
并行ORT累计时间不能直接除以wall当端到端占比，wall减ORT也不是网络时间。
普通性能运行关闭诊断日志，保留结构化统计；故障定位才显式打开 debug/timing。
逐包 `NDNSF_EXACT_DATA_DISPATCH` 明细还要求
`ndn_service_framework.ServiceProvider=TRACE` 和控制计时开关。
阶段统计和签名清单证据的原有日志级别不变；普通 WARN/INFO 不写逐包发送明细。

每次按 run + request/attempt + role + epoch 过滤统计，禁止把追加文件中的旧PP结果当本轮TP结果。
有token事件不等于请求/checkpoint成功；逐token参考、实际设备执行、完整role receipts和清理分别报告。
对象字节、wire字节、物理链路字节、face.put→onData延迟、应用delivery及RTT使用各自边界。

## 7. SSH、失败与收尾

VPN Connected 不证明 SSH可达；TCP22超时不证明账号封禁。
按 DNS/route → TCP → host-key握手 → 认证 → 远端命令分段诊断，保留有限输出，不打印凭据。
用户重连VPN后可立即复查。收到IT故障邮件时保存来源/接收日期，不能推断作业已死；
恢复后先查原 job、退出码和产物。

按照授权的600秒间隔和等待窗口，以单owner、有界命令续查。
一个轮询窗口结束不是实验完成；先查旧进程/作业再从检查点续开，响应不明不重复提交。
本文更新不授权启动SSH轮询、构建或作业。

错误后保留首个失败边界，修消费该输入的最小路径，再重验受影响范围。
区分调度、quota/mount、源码、ABI、模型、网络、oracle和清理；不靠扩大timeout掩盖确定性错误。
Response、leader退出、Slurm COMPLETED 与进程组清理不是同一事实。
down/recovery 绑定原job/node/boot及进程身份；拿不到原节点审计就保持 needs_closeout。

关闭且证据已归档后，按精确路径 allowlist 回收旧SIF、闲置构建/传输副本和过期缓存。
保留最新合格且仍被引用的模型、Repo、私有状态、运行目录与调查子集；检查进程、mount、锁、
symlink和文件身份。不得 blanket rm /var/tmp，也不得修改系统swap来让实验通过。
