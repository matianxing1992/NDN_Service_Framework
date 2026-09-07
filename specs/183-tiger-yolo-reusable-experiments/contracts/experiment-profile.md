# Experiment Profile And Run Contract

**Status**: planned; proposed files/CLI below do not exist yet.

## Operator Interface

唯一 planned 入口 `Experiments/TigerCluster/jobs/yolo/submit.py`，统一参数 `--profile PATH --run-id ID --output PATH`。子命令：

| Command | Semantics |
| --- | --- |
| `check` | 本地只读校验及 resolved argv/mounts/缺口报告；不创建远端目录、上传或调用 Slurm |
| `prepare` | 检查 input closure 后产生新的本地不可变 bundle/run-plan；不执行模型或 sbatch |
| `local --case local-cpu` | 已满足 gate 后运行精确 SIF CPU 诊断；不等于 Slurm PASS |
| `submit --case single-node-gpu|two-node-gpu|negative-dependency` | 登记 candidate/gate 独占、重新验证当前 bundle 并提交一次；不能隐式自动进入下一 case |
| `collect` | 从该 run 已有证据重算 verdict；不启动缺失步骤、不将原失败覆盖为成功 |

验证阶段参数 `--stage inputs|runtime|dispatch` 仅用于 check/prepare；不允许借较早 stage 绕过 submit 所需 dispatch gate。凭证路径引用可配，内容不进入 argv/环境 dump。应用参数禁止 `eval`，构造 argv 数组；路径以 profile 文件目录为基准解析。

## Profile Fields

唯一可编辑 profile：`profiles/yolo-two-node.json`，schema `tiger-yolo-v1`。不得提交可执行的空 hash/占位符配置；T001/T004 填齐真实输入后才产生 enabled profile。以下字段是接口合同，不是已测默认值。

| Section | Required content / validation |
| --- | --- |
| `schema`, `profileId` | 精确版本，稳定非空标识；未知字段/类型错误拒绝 |
| `release` | source lock/ref+sha256、runtime manifest+sha256、SIF+sha256、library lock、host-gate、local-SIF gate；按 stage 要求完整 |
| `workload` | YOLO26n package、模型版本/导出参数、各 shard/完整模型/input/oracle hash；现有 DI policy/plan descriptor 引用及 schema；不得写死最终伪造 Selection |
| `runtime` | 容器 Python/native entrypoints、显式 cwd `/bundle`、受控 PATH/env、Apptainer path/semantic version；container root/mount map |
| `cluster` | 实际 partition/account/constraint、nodes=2、gpusPerNode=1、GPU class、cpusPerNode、memoryPerNode、wallTime、tcpPort、共享 project root/scratch selection；所有值先核实 |
| `roles` | BackboneNeck=A、DetectShard0=B、DetectShard1=B、Merge=A；每角色唯一 Provider identity/cert locator/PIB；模型阶段 cuda device0；Merge host CPU 明示 |
| `security` | Controller/user/provider 角色规则、公开 trust root/cert/policy hashes、私有材料 locator、允许服务、状态刷新/epoch；不存私钥/token |
| `timing` | 设计起点 startupSeconds=120、ackTimeoutMs=1500、requestDeadlineMs=60000、progressTimeoutSeconds=30、cleanupSeconds=30；正数且 ACK < request，全部传到实际入口；不支持字段需修接口或拒绝 |
| `cases` | local-cpu、single-node-gpu、two-node-gpu、negative-dependency；每个独立 effective case digest，角色数/CPU mode/故障差异显式登记 |
| `schedule` | 单节点 1 warmup+1 measured；每个正常双节点 job 1 warmup+3 measured，串行；两个独立正常 allocation。failure case 单独 request/record |
| `oracle` | 与当前数值合同一致的 fixture/preprocessing/schema/order/filter/atol/rtol/hash，不可运行时覆盖 |
| `storage` | SIF/model CAS roots、绑定位置、peak bytes+margin、结果路径规则、清理 allowlist；不复制大 artifact 到每次 run |
| `evidence` | collector/contract/harness hash、required cases/roles/events、raw retention/hash/secret redaction、durable result destination |

15 分钟/job 是起始资源预算建议，必须 T001 按 warmup、4×deadline、startup、stage/hash/cleanup 计算余量并登记；不满足预算时在提交前拒绝，不能运行中加时。GPU 型号/内存等不在本轮凭空填成“已验证”。
正常请求默认不启用 response-level reselection；协议已有有限传输重试保持锁定。实例故障时失败而不切换另一 Provider 来凑 PASS。

## Candidate Identity

I = digest(四库 exact revisions+source seals、依赖/工具链/base/build definition、wheels)；R = digest(I、最终 SIF hash+native/library manifest)；E = digest(R、所有执行脚本/harness、profile 的有效行为字段、模型/input/oracle、安全规则、validation contract)。
阶段化检查：inputs 需要 I 和构建所需现存文件，runtime 需要 R，dispatch 需要 E 和之前 gate receipts。profile 文件自身摘要单独记录；E 以明确字段集计算，不将 E 自身/未来 result hash 纳入自身摘要。input→runtime→experiment 为单向引用，不循环。
身份私钥每 run 单独生成不属于可复用候选；trust-policy 与生成规则固定，公开证书摘要属于 ResolvedRun。分配的 host/IP/GPU UUID、run ID、物理 artifact 位置进入 ResolvedRun，不改变 E；物理文件内容必须仍匹配 E。任何超时/角色/容差/GPU class/env 行为变更都改变 E。
检查后执行必须使用只读/不可变 bundle；提交前核对 bundle inventory，worker 再验证关键输入。路径别名不能让 checks 检 A、exec 跑 B。

## Topology And Data Rules

两计算节点 hostname 必须不同，GPU UUID 各自取 allocation/container 实测。四 Provider 身份各不相同；同节点多个角色可以共享一个 GPU，但各自内存/ready/exec 证据独立。
Node A：NFD、Controller、User、Repo 发布/取用入口、BackboneNeck、Merge。
Node B：NFD、DetectShard0、DetectShard1。
Backbone 结果到 B 的两 head；head 结果到 A 的 merge；边名称/生产者/消费者及 plain tensor digest 由现有合法执行记录关联，不能把密文 hash 当成数值内容 hash。私有 tensor 不写到公开日志。
脚本设置 network substrate，不在 shell 中实现 DI 调度或假冒 runtime Selection。模型工件允许预暂存但必须如实计为 warm/prestaged，不宣称冷 Repo 获取。原始输入与依赖激活仍经实际安全 NDN 路径。

## Runtime And Submission State

`PREPARED → SUBMITTING → SUBMITTED(jobId) → RUNNING → PASS|FAIL|INCOMPLETE`。
提交前原子建立 candidate/gate 活动记录；两并发启动仅一个可到 sbatch。sbatch 返回后网络断开导致 job ID 未知时进入 `SUBMISSION_UNKNOWN`，恢复按唯一 comment/run ID 查询已有 job，未确认无提交前不重试。终态只写一次，reanalysis 是独立文件。
共享锁位置由 profile 指定，必须对所有操作者共享可见；纯本机锁不能声称阻止另一机器重复提交。allocation 阶段身份/路径校验失败不得启动 Provider。
wait/readiness 使用 monotonic deadline，并检查 child 和 peer failure。Controller/Provider 长期进程正常受控 stop 与异常提前退出分开记录；client/numeric checker 必须正常 exit0，cleanup 必须 reap 且无残留进程。强制 kill/写盘失败不得给 clean PASS。
