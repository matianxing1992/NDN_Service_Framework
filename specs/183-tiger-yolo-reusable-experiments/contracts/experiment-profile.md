# Experiment Profile And Run Contract

**Status**: T004 partial — schema、只读 `check`、确定性运行预览和提交记录组件已实现；
完整五命令、合格不可变 bundle、实际 enabled profile 和生产提交接线尚未完成。
没有启动资格。

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

### Concrete schema and structural checkpoint

`schemas/tiger-yolo-v1.schema.json` is the exact field/type contract. All objects
reject unknown fields; launch consumers must also prove every supported field is
used (T004/T007), not merely accepted by the schema. File references have exactly
`path`, integer `bytes`, and `sha256`. `release.inputs/runtime/dispatch` reference
the I/R/E plane manifests; `release.gates` holds independent receipt references.
Transitive source/model/security details live in those hashed owner manifests,
not duplicated manual settings. `evidence.operatorLock` references the local
operator dependency lock; that lock is not installed into the SIF.

`load_operator_profile` validates structure, stage manifest presence, identity
namespace, paths and an arithmetic walltime lower bound. It does not check that
referenced artifacts exist, their hashes, receipts, or site resource availability.
The result explicitly reports `integrity=NOT_EVALUATED` and
`qualification=NOT_EVALUATED`; it must never authorize build/upload/submit.
Local relative paths are resolved against the profile directory; remote storage
paths must be absolute and are not probed here. Symlinks, control characters and
ambiguous bind-path separators are rejected. Runtime paths inside the container
remain `/bundle`, `/output`, and the sealed executable paths, not host paths.

Concrete resource units are `memoryGiB` and `wallTimeSeconds`. Explicit
`timing.stagingSeconds` budgets allocation staging and hashing; the conservative
per-allocation lower bound is staging + startup + ceil(4 × requestDeadlineMs /
1000) + cleanup. With the fixture values 120/120/60000/30 this is 510 seconds,
not a measured duration or a verified production default. Progress timeout is
bounded by the request budget; it is not added as another serial allowance.
Later coordinator/collector time must fit registered stage/cleanup budgets, and
production consumption is audited at T007. Normal allocations are separate jobs,
not twice this bound in one job.

`documentDigest` is a canonical JSON fingerprint, not E. The generated effective
behavior document for E must exclude its own dispatch/receipt references and
physical locations; prepare must avoid an E→profile→E self-reference. Runtime
and dispatch checks must additionally verify their content-plane ancestors and
genuine prerequisite receipts. No actual executable profile is supplied while
the base/model/signature inputs remain missing.

## Candidate Identity

I = digest(四库 exact revisions+source seals、依赖/工具链/base/build definition、wheels)；R = digest(I、最终 SIF hash+native/library manifest)；E = digest(R、所有执行脚本/harness、profile 的有效行为字段、模型/input/oracle、安全规则、validation contract)。
阶段化检查：inputs 需要 I 和构建所需现存文件，runtime 需要 R，dispatch 需要 E 和之前 gate receipts。profile 文件自身摘要单独记录；E 以明确字段集计算，不将 E 自身/未来 result hash 纳入自身摘要。input→runtime→experiment 为单向引用，不循环。
身份私钥每 run 单独生成不属于可复用候选；trust-policy 与生成规则固定，公开证书摘要属于 ResolvedRun。分配的 host/IP/GPU UUID、run ID、物理 artifact 位置进入 ResolvedRun，不改变 E；物理文件内容必须仍匹配 E。任何超时/角色/容差/GPU class/env 行为变更都改变 E。
检查后执行必须使用只读/不可变 bundle；提交前核对 bundle inventory，worker 再验证关键输入。路径别名不能让 checks 检 A、exec 跑 B。

## Content Plane Integrity Format (T002 partial implementation)

`runtime/yolo_profile.py::check_plane`与`check_chain`实现内容完整性子层；输入是生成的只读清单，不新增操作者配置，也不返回运行PASS。格式为`schema=tiger-yolo-plane-v1`，精确字段`stage,parentId,files,parameters`；stage为inputs/runtime/dispatch。每个files行包含相对path、整数bytes、sha256摘要，parameters全量参与摘要。文件路径不参与ID，允许同字节输入搬迁；行为参数、文件逻辑名/字节数/hash参与ID。拒绝重复JSON键、NaN、未知顶层/行字段、非普通文件、符号链接、路径越界和文件变化。每次check_chain重新校验前驱并核对parentId，不信任调用者记忆的I/R。

每平面最低文件角色：inputs为sourceLock/sourceSeal/buildDefinition/baseSif；runtime为sif/nativeManifest/libraryLock；dispatch为effectiveProfile/harnessManifest/modelManifest/oracle/fixture/trustPolicy/validationContract。完整的源码archive/wheels/所有harness文件等仍须由各专属validator解析并验证，最低集合不能代替传递依赖完整性。文件在其清单根下；接收工具应在包含CAS与bundle的共同artifact根生成清单，不为此按run复制大文件。

返回`integrity=VERIFIED, qualification=NOT_EVALUATED`；不检查证据是否真实执行、有效字段是否被launcher消费、模型签名或库ABI。因此该返回值不能授权prepare/build/submit。原source-sealer/handoff validator、后续YOLO receipt/配置解析与所有外部操作前重新检查仍是T002/T004的未完成部分；这里没有宣称零副作用生产入口测试完成。receipt不得进入自身候选摘要而产生循环引用。

## Topology And Data Rules

两计算节点 hostname 必须不同，GPU UUID 各自取 allocation/container 实测。四 Provider 身份各不相同；同节点多个角色可以共享一个 GPU，但各自内存/ready/exec 证据独立。
Node A：NFD、Controller、User、Repo 发布/取用入口、BackboneNeck、Merge。
Node B：NFD、DetectShard0、DetectShard1。
Backbone 结果到 B 的两 head；head 结果到 A 的 merge；边名称/生产者/消费者及 plain tensor digest 由现有合法执行记录关联，不能把密文 hash 当成数值内容 hash。私有 tensor 不写到公开日志。
脚本设置 network substrate，不在 shell 中实现 DI 调度或假冒 runtime Selection。
本 Spec 的正常路径沿用当前 YOLO User 的 encrypted canonical publisher 和 native
Provider 的 canonical assembler：模型源包只供发布方读取，Provider 经 NDN 获取
模型对象并在各自可写 cache 中组装。新 worker 不要求、不挂载 Provider `/artifacts`
或人工预切分模型目录；`--artifact-cache-dir` 由实际应用参数 owner 绑定到该角色
的 `/output/artifact-cache`。共享 bundle 只含审核后的代码/配置/公开材料，不能
夹带模型参考输出。原始输入、依赖激活和结果仍走实际安全 NDN 路径。warmup 后
Provider 缓存命中必须如实记录，不能称每请求都冷获取。未来若增加预暂存 case，
需显式变更行为、失效证据并重验，不能临时挂目录。

## Runtime And Submission State

`PREPARED → SUBMITTING → SUBMITTED(jobId) → RUNNING → PASS|FAIL|INCOMPLETE`。
提交前原子建立 candidate/gate 活动记录；两并发启动仅一个可到 sbatch。sbatch 返回后网络断开导致 job ID 未知时进入 `SUBMISSION_UNKNOWN`，恢复按唯一 comment/run ID 查询已有 job，未确认无提交前不重试。终态只写一次，reanalysis 是独立文件。
共享锁位置由 profile 指定，必须对所有操作者共享可见；纯本机锁不能声称阻止另一机器重复提交。allocation 阶段身份/路径校验失败不得启动 Provider。
wait/readiness 使用 monotonic deadline，并检查 child 和 peer failure。Controller/Provider 长期进程正常受控 stop 与异常提前退出分开记录；client/numeric checker 必须正常 exit0，cleanup 必须 reap 且无残留进程。强制 kill/写盘失败不得给 clean PASS。

### T004 implemented interface checkpoint

当前 `jobs/yolo/submit.py` 仅开放 `check`，`--profile` 必需，`--stage` 默认
`dispatch`。可同时传 `--run-id/--output/--case` 三项查看确定性运行预览；不传
则只做当前阶段内容检查。未知字段、错 manifest/hash、缺阶段以 exit 2 拒绝。
内容匹配仍返回 exit 78、`status=INCOMPLETE`、`qualification=NOT_EVALUATED`：
source/model 专属校验、生产调用/挂载和真实 receipt 尚未接入，不能构建或提交。
`prepare/local/submit/collect` 暂不开放，而不是提供能绕过门槛的占位执行器。

`check` 的内容检查范围为当前及前驱 I/R/E 平面，先核对 profile 对平面清单的
bytes/hash，再调用原 `check_chain`，最后再次核对清单引用。它不把最低文件
集合当作完整传递依赖清单。读取过程中不创建 output/cache/日志目录，不操作
SSH、Slurm、容器或模型。Python3.8 的 JSON Schema 导入可能执行 stdlib 的
只读 `uname -p`；回归明确允许这一个探测，不放开实验启动/网络/文件修改。

运行预览是 `PLANNED`，不是冻结 bundle 或 DI Selection。它复用 `assigned_roles`，
包含独立 role identity 名、4 Provider 的预期 rank/device、逐请求 ID/输出和未解决项；
真实 allocation 留为 null。local-cpu 和 single-node-gpu 使用 1 warmup+1 measured；
正常 two-node-gpu 使用 1+3；negative-dependency 只运行一个独立负例请求，不计入
正常成功样本。请求 ID 由版本化 domain、run namespace 和请求序号的 SHA-256
前 128 位确定性生成；runId 不重复，命名不依赖随机线程/机器状态。

`caseBehaviorDigest` 绑定配置行为、注册 case/角色/schedule；不等于 candidate E。
runId、物理路径、profileId 标签、release/未来 receipt 引用不进入该摘要；父 R 与
实际 I/R/E 关联仍由内容链负责。`documentDigest` 另外绑定原 profile 文档。
从不同 cwd 查看相同 run 得到同一预览；更换 run 不改变行为摘要，但生成不同
角色/请求名。换行为参数会改变摘要。实际 argv、mount、signed material、
allocation 和 qualification 继续明确列为 unresolved，不宣称已消费全部字段。

### Expected-rejection terminal record

The `negative-dependency` case has a separate terminal contract; it MUST NOT be
reported through the normal success verdict. `runtime/yolo_result.py::finalize_expected_rejection`
requires a retained `tiger-yolo-expected-rejection-v1` record bound to the exact
run, request, attempt, candidate digest, and request deadline. The record must
show one committed Selection with `reselectionCount=0`, a specific planned edge
that failed after Selection (`DEPENDENCY_DATA_MISSING` or `PEER_FAILURE`), no
successful response, and a `CLEANUP_COMPONENT_ONLY` record proving all owned
children were reaped without forced cleanup. A timeout, missing file, generic
nonzero exit, or an operator-supplied PASS marker is insufficient. The helper
returns `EXPECTED_REJECTION_PASS` only for this exact component contract; a
real MiniNDN/Tiger negative run and the production collector remain required.

`runtime/yolo_submission.py::SubmissionJournal` 只管理共享提交记录，不调用
sbatch，也不验证模型。所有操作者必须用同一已验证共享目录；本机 flock 测试
不能证明 Tiger 共享文件系统语义。每 candidate/gate 记录通过有界 2 秒 flock、
同目录临时文件、fsync、原子 replace 和目录 fsync 更新。重复 runId 不复用；
旧终态保留。将 `SUBMITTING` 持久化成功后才能进入唯一 sbatch 调用。

进程在 SUBMITTING 崩溃或响应丢失时不能重提；按唯一 submissionKey/comment
查询，零匹配仍为 SUBMISSION_UNKNOWN，单一 jobId 才接回 SUBMITTED，多匹配
明确报错并继续占用。query transport 和真实 Slurm 输出解析仍待接线。
尚处 PREPARED 可原子转为 `CANCELLED_BEFORE_SUBMIT` 释放预留；这是提交记录的
终态，不是模型结果。进入 SUBMITTING 后禁止此取消出口。正常 finish 必须由
上层先核对同一 job 真正终止和 collector verdict；jobId 不匹配、改写终态、
未知提交直接 finish 均拒绝。正式提交入口未实现，所以这些组件不构成 T004 完成。

### Application Sync name and startup coordination

The canonical rule is `applicationName + "/sync"`, where `applicationName` is
already the application's absolute NDN instance namespace (for example,
`/appname` becomes `/appname/sync`). It is not a Provider identity or the
template's display label.
The resolved plan records `applicationName`; preparation records the same value
as `runtime.application_name` and sets `group = applicationName + '/sync'`.
For isolated Spec183 runs the default application name is the unique run
namespace. Provider identities and `provider_prefix` remain separate inputs.
Controller, User, Provider and NFD Sync forwarding must consume this exact
`group`. Do not infer it from Provider names or copy the legacy CPU baseline's
hard-coded `/group` route. The future network setup must install the actual
application Sync prefix; this is not yet an executed forwarding gate.
`configure_network` now performs the owned NFD/nfdc command chain and publishes
run-bound route readiness only after successful status, endpoint agreement,
route/strategy commands and listings. It consumes the pinned profile TCP port
and allocation-derived endpoints; signed peer probing still establishes actual
data-path readiness. Management uses an idle Provider HOME, no GPU/model mount.

`StartupBarrier` coordinates only control/readiness records in an exclusively
created run directory. Each record binds run ID, candidate digest, probe ID,
stage and rank; atomic no-overwrite publication prevents partial/stale reads.
All waits share one monotonic startup budget and check owned-process/peer
failure. `start_workload` requires route-ready records with the exact namespace
and application Sync prefix, then two directional signed network receipts,
Controller publication/Repo readiness, and the complete native Provider set.
It never fabricates route-ready records. Single-node cases skip only the
two-node network probe. The outer worker still owns NFD setup, final cleanup,
requests and full result collection. RUNTIME_READY is not inference PASS.

### Numerical response evidence

Numerical evidence: the fixed public benchmark may retain one authorized User
response per invocation in `yolo-response.bin` (exclusive, 0600, ≤1MiB), never
in a Provider mount, log or Git. General application retention remains disabled.
Its byte count/hash and candidate/request/attempt/plan binding accompany the
numerical record. Collection must recompute the frozen reference comparison
from those bytes, not accept a `matched` flag alone. Reference authenticity and
candidate-pinned decoder/oracle implementation remain collector preconditions;
numerical-component acceptance is not an end-to-end or GPU verdict.

### Frozen harness file contract

`runtime/yolo_bundle.py` 实现小型脚本 bundle 的 freeze/verify。清单格式为
`schema=tiger-yolo-harness-v1, files={relative-name:{bytes,sha256}}`，明确登记
15个运行脚本/schema/operator-lock 文件（包含实际跨节点探测apps/yolo_network.py），不递归复制仓库。清单生成物可放在
工作树外，通过显式source_root查找同一批已绑定字节；不会为了清单改源码目录。
缺少真实 `apps/yolo.py`、`yolo_result.py`、`run.sbatch` 时仍不得构造生产 bundle，
不得写假实现来填清单。清单完整性不能代替T007实际import/调用闭包审查。

冻结前先验证全部输入，单文件最多4MiB、合计16MiB，仅用于小型脚本边界，
不是SIF/model容量阈值。拒绝未知路径、链接、hash错误、非文本/NUL和可识别
的PEM私钥块；内容检查不是通用秘密检测，仍必须审查清单来源和脚本内容。
模型/输入/oracle、私有角色材料、宿主.so/venv都不在此清单或共享目录中。
公开配置/证书与发布方包继续由其既有owner管理并单独绑定，不靠全仓复制夹带。

将核对过的字节复制到新目录，不用指向可变工作树的hardlink；源之后变化不
影响冻结副本。文件fsync、manifest最后写入，文件置0444/目录0555并再验证。
已有目标绝不覆盖；部分失败保留原目录，不能当作有效bundle或在原目录重试。
verify仅扫描已登记目录，未知子目录当场拒绝，不先遍历其中可能巨大的内容。
权限位不是对目录所有者的密码学保护，实际worker仍须在使用前复核并只读挂载。

`check --stage dispatch` 已验证profile中的harnessManifest与E平面同一bytes/hash，
并检查冻结树的完整内容、无额外文件和只读模式。成功只增加
`harness.integrity=VERIFIED`；整体仍INCOMPLETE/NOT_EVALUATED，因为业务/源/模型/
ABI/receipt等生产门未完成。此实现不开放prepare/submit的资格绕过入口。
