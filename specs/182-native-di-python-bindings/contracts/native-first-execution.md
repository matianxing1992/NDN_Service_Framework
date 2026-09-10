# Native-First Execution Order

**Revision**: 7 | **Date**: 2026-09-10 | **Status**: PLANNED

用户确认的剩余执行顺序；覆盖旧文档中“所有真实跨进程用例推迟到 T016”及
“生产 requester 进程内持有 artifact authority 私钥”的规定。保留原 17 个父任务、
FR/SC/PO 和有效历史证据，不重做 Spec182，不将本轮设计修订计为产品行为通过。
本文件定义剩余阶段与新卡；[tasks.md](../tasks.md#execution-progress) 是状态唯一入口。

## Stage Order

| Stage | Required exit before advancing | Units / parent ownership |
| --- | --- | --- |
| N1 Independent artifact authority | authority 在 requester 之外持有签发私钥与受保护工件密钥；C++ authenticated grant 正反例通过 | R11-B1 / T005 |
| N2 Cross-process unary | C++ `DI_NativeRequester` → Core → C++ `di-native-provider`，实际 ACK、Selection、handler 执行及最终 Response 和数值 oracle 通过 | R11-B2 / T010 |
| N3 Same-chain stateful behavior | 沿 N2 原生生产链依次完成 stream、continuation、recovery、replacement、cleanup 正反例 | R11-B3--B7 / T010,T011,T009 |
| N4 Maintained caller migration | 16 个已登记旧调用方逐个有 native 路由和实际行为证据；Python 只作薄封装/兼容入口/外部编排 | R11-B8 / T012,T013 |
| N5 Closure and qualification | no-Python 与依赖闭包工具、T015 收敛、T016 完整资格，最后 T017 交接 | R11-B9 / T014--T017 |

顺序为 N1 → N2 → N3 → N4 → N5。下一项为 R11-B1，不再以一次全仓库扫描代替
该明确缺口的修复。保留 R10-B83/B84 的 C++ owner/name 修复；局部 PASS 不能替代 N2。
N3 是能力阶段，不是一次吞并五种状态行为的大批次。每张卡形成稳定出口即进行批末验证。

## Native Validation Order

每小任务：C++ production code → 独立官方 review-agent 只读静态门 → 同批下一任务。
整批逻辑/流程审查后：**C++ unit/integration/process tests → Python wrapper checks**。
先完成对应 C++ 生产逻辑与行为证明，再验证 Python 对它的参数、异常、寿命和转发。
五 lane、review trace、Closure decision 和四类 miss retrospective 复用共享
[batch contract](../../../skills/speckit-code-design/references/batch-quality-gates.md)。

原生行为的断言主体、fixture/driver 与 oracle 必须是 C++，直接调用具名生产 target。
Python 可以搭建外部网络或启动 C++ executable，但不得计算 planner/成功判据/恢复状态。
Python 测试数量、py_compile、CLI help、check-only、链接烟测、fake ACK/预置 Response、
同进程真实 Provider 都不能推进 N2/N3、native 父任务或 T016 状态。
批末定向 process tests 是开发验收，必须在迁移调用方之前执行；T016 仍负责最后的
完整同源 C++ unit → integration/process → MiniNDN/no-Python → Python wrapper suites。
没有重跑的新源码不能继承不兼容旧二进制的 PASS；有效未受影响证据按原范围复用。

Placement 的伸缩契约按 **role** 与 **Provider identity** 分开：admitted offer
观察值对每个 Provider 只能有一份，但一个 Provider 可以承载多个 execution role。
策略先尽量把角色分散到不同 Provider，资源不足时才允许容量约束内的 co-location。
CPU 角色可以共享 Provider；GPU 角色必须使用不同的 offer-scoped device，并分别通过
该设备的独占检查和同一 Provider 的累计显存检查；累计 reservation 溢出会 fail closed。
sealing、role-specific grant、
Selection projection、group capability 与 Provider-local handler 都必须继续按 role
索引，不能因 Provider 相同而合并角色。维护中的 Qwen profile 仍有固定的三阶段顺序和
rank/tensor contract；这些 adapter 限制不等同于 deployment 的 Provider 数量限制。

## Multi-Machine Runtime Boundary

MiniNDN 常把每个节点的 `HOME`、PIB/TPM 和当前工作目录预置在同一进程环境中，因而不能
代替多机启动器验证身份隔离。Spec110 的 Slurm topology launcher 读取的 `identityRef` 是
只读身份源；启动器必须在实际节点上为每个 Controller、Provider 和 User 复制独立的
`.ndn/pib.db` 与 `.ndn/ndnsec-key-file` 到作业 scratch 下的进程专属 `HOME`，显式设置
对应 `NDN_CLIENT_PIB`、`NDN_CLIENT_TPM` 和 `NDN_CLIENT_TRANSPORT`，并清除继承的 NDN
keychain 环境。NFD 使用独立 scratch `HOME` 且不继承角色 keychain。复制或必需文件检查
失败发生在进程 `exec` 之前；READY 日志不能把它升级为成功。该边界由
`allocation_topology.render_process_launcher` 和
`run-allocation-topology.sh` 共同实现，避免同一只读身份或登录节点环境在多机上被复用。

同一节点上的 NFD、Controller、User 和多个 Provider 由独立 Slurm steps
并存运行；这些 steps 必须使用 `--overlap --exact --ntasks=1
--cpus-per-task=1`，禁止 `--exclusive`。后者会把 NFD 或第一个 Provider
变成整节点资源占用，后续同节点进程可能持续排队；Provider 的 GPU 仍由
`--gpus-per-task=1 --gpu-bind=map_gpu:<gpuRank>` 与 UUID 检查单独约束。

`nodeRank` 还必须绑定 Slurm 的真实 allocation order。supervisor 和独立的
route-configuration entry point 在任何 NFD 启动或 route retry 前，都要把
process map 中按 rank 排列的 node names 与
`scontrol show hostnames "$SLURM_JOB_NODELIST"` 的结果逐项比较；缺少、为空
或顺序不一致时以 `SPEC110_ALLOCATION_NODE_ORDER_MISMATCH`（或更具体的
nodelist preflight error）在 pre-start 失败。`srun --relative=<nodeRank>` 使用的
就是该顺序，MiniNDN 中稳定的创建顺序不能证明真实 Slurm 多机映射正确。

此启动器的离线 fake-binary 测试可以用 `NDNSF_SPEC110_TEST_MODE=1` 跳过不存在的 fixture
身份源，但该开关不属于生产部署，也不能作为 T016/T017 资格证据。真实多机资格仍须在
目标节点验证 identity、NFD TCP/UDP route、依赖库和工作目录的候选绑定。

本轮静态复核还冻结了三个容易被 MiniNDN 掩盖的节点绑定：若命令参数再次携带
`identityRef`，启动器必须把精确的 `--identity PATH`／`--identity=PATH` 参数改写为该进程
专属 `HOME`；Provider 在 `exec` 前必须读取单值 `CUDA_VISIBLE_DEVICES`，再让
`nvidia-smi -i` 返回的 UUID 精确等于 map 中的 `gpuUuid`，不能只相信 `--gpu-bind` 的编号
或“该 UUID 在全机列表中存在”；TCP/UDP 诊断必须分别使用各自节点端口，不能用已选传输的
端口冒充另一种传输。失败均停在 pre-exec/probe 边界，不得生成 READY 或资格 PASS。
NFD socket 还必须位于当前作业的 `--scratch` 目录内；仅有 `/tmp/ndnsf-di-*` 前缀而
跨作业复用或含有 `..` 路径组件的 process map 会在启动前被拒绝，NFD state directory
也执行同样的路径组件检查。真实 Slurm allocation 中 scratch basename 还必须是
`ndnsf-di-<SLURM_JOB_ID>` 或以 `ndnsf-di-<SLURM_JOB_ID>-` 开头；离线 test mode 才允许
fixture basename，避免清理旧 socket 时误伤其他作业。
在启动 NFD 前，supervisor 还必须在每个对应目标节点将声明的 IPv4 address 绑定到临时端口；
地址格式正确但未分配给该节点时，以 `SPEC110_NODE_ADDRESS_NOT_LOCAL` 在 pre-start 失败。
这只是节点地址归属门，不替代后续 NDN face/route 连通性验收。

候选镜像的 ELF 闭包还必须在各自的 assembler/devel 阶段通过
`verify-runtime-closure.py` 的宿主路径门禁。门禁同时读取 `DT_RPATH`/`DT_RUNPATH`
和 `ldd` 的已解析路径，并拒绝 `/home/`、`/workspace/`、`/build/`、`/src/`、`/tmp/`
等宿主或构建临时前缀；最终 runtime stage 继续执行常规 unresolved-library 闭包检查。
这只证明构建产物没有带入这些宿主路径，不等同于 SIF、驱动注入或真实 Slurm 多机资格。

生成的 process launcher 也不能假定 `evidence` 或提交节点文件系统在每个计算节点可见。
`run-allocation-topology.sh` 必须先用目标节点的 `srun` 将每个 launcher materialize 到该作业
的 scratch，再从该节点路径执行；远端不可见时应在任何 NFD/业务进程启动前失败。
NFD 的 `--config` 参数也必须改写到该作业 scratch 的配置副本，禁止固定 `/tmp` 配置在作业
之间复用；配置文件写入和 launcher 使用必须指向同一节点本地路径。
每个 NFD 启动前还必须在目标节点删除对应 scratch socket，并按 node rank 保存其 `srun` PID；
readiness 同时要求 PID 存活和新 socket 出现，不能用复用 scratch 中的旧 socket 放行。
同一启动器还必须在启动 NFD 前用目标节点的 `srun` 验证显式 `--workdir` 可见；提交节点存在
而计算节点不存在的 bundle 不能进入 readiness 阶段。
同样，在启动任何 NFD 前必须在每个目标节点检查所有非 NFD `identityRef` 的
`.ndn/pib.db` 与 `.ndn/ndnsec-key-file` 可读；身份源只在提交节点可见或不可读时，必须在
pre-start 边界失败并保持零 NFD 启动，不能等业务进程 `exec` 后才暴露绑定错误。
identity source 本身及其 `.ndn` 树不得含符号链接；supervisor 在 NFD 启动前检查，launcher 在
copy 前再次检查，避免 scratch `HOME` 通过链接回到共享 `/project`。
process map 的 `(address,tcpPort)` 与 `(address,udpPort)` 端点不得重复；启动前由目标节点
执行 IPv4 TCP/UDP bind probe，若已有监听则以 `SPEC110_PORT_NOT_AVAILABLE` 在 pre-start
失败。该 probe 不能替代跨作业端口租约，probe 关闭后的竞争仍由 NFD 最终 bind 处理。
在任何子进程启动前发生的可见性、materialization 或 map-render 失败也必须写出包含原始退出码
和 `survivors: 0` 的 `teardown.json`，不能以无证据的直接退出代替失败边界。

多机 transport probe 不依赖节点预装的可选 `netcat`；诊断脚本使用节点上已有的
`python3` socket API 做有界 TCP/UDP connect。该 probe 只用于 selected/diagnostic transport
的连通性屏障，不能把 connect 成功解释为 NDN 协议或业务请求成功。

## Independent Authority Boundary

- **Owner**: 复用 `NativeArtifactGrantIssuer`/现有 policy、grant wire、签名和 recipient
  encryption；部署到独立 C++ authority 进程。独立的不只是类名或 identity 字符串。
- **Requester**: 保留自己的签名私钥、authority 公钥/信任锚和寻址配置；不得读取、
  注入、继承或在配置中要求 authority 签发私钥。不得构造本地 issuer 为自己补发 grant。
- **Authority**: 持有 issuer policy、签发私钥和工件密钥，独立核对已认证 requester、
  provider recipient、request/attempt、plan/grant-view/model digest、epoch、purpose、
  residency 和有效期；返回既有签名与 recipient encryption 保护的 grant。
- **Transport**: 用既有 Core signed application Data/publication/fetch 能力连接 requester
  grant client 与 authority；不创建平行 DI 协作协议、不改 Request/ACK/Selection/Response。
  R11-B1 编码前冻结 transport adapter 的实际符号/名称、配置 schema 兼容错误、timeout/
  cancel、Face/executor 生命周期及 target/source closure；不得以待选端口直接开始编码。
- **Isolation proof**: 独立 PID 只是必要条件。验收使用不同密钥目录及权限/隔离环境，
  验证 requester 无 authority 私钥文件访问和继承 FD；不在日志输出密钥内容。
  requester 配置 authority 私钥或回退到进程内 issuer 必须被生产入口拒绝。
- **Failure**: authority 不可达/拒绝/超时、错签名/recipient/epoch/绑定必须 fail-closed；
  不产生可执行 Selection，不激活受保护工件，不恢复 plaintext 或本地自签 fallback。
  已有 Core Controller 权限撤销保持；本次不顺带重设计独立工件撤销协议。

## Dispatch Cards

所有卡均为待执行实现/验证义务。测试 target/selector 的新名称在卡片实现前登记为
PLANNED 并核对注册，不把尚不存在的测试名写作已执行命令。共同 Read：CD-004、
proof-design、既有 state contracts、R10-B82/B83/B84 evidence 和调用方。

| Unit | Depends | Read / Write boundary | Required result and independent counterexample |
| --- | --- | --- | --- |
| R11-B1 Independent Authority | T001 有效设计/依赖关闭；T005-A/B 现有实现作输入 | `examples/DI_NativeRequester.cpp`、NativeArtifactPolicyAuthority/NativeAuthenticatedGrantClient、生产 C++ authority 入口和 Waf target；C++ grant tests/config | 冻结端口后移除 requester 私钥/本地 issuer；独立进程签发、真实 grant 验证成功；错误 recipient/签名、私钥配置、authority 拒绝/不可达失败且无本地 fallback |
| R11-B2 Native Unary Process | R11-B1；T008/T009/T010 已有实现作输入 | `DI_NativeRequester`、`examples/DI_NativeProviderExecutable.cpp`、NativeInferenceClient/ProviderHost、C++ process driver/oracle 和测试注册 | requester、authority、Provider 为独立进程；真实 protected model request 经 ACK→Selection→handler→Response，结果与冻结输入/数值 oracle 相符；停止 Provider/拒绝授权不得伪成功 |
| R11-B3 Native Stream Process | R11-B2 | 同一 CLI/library/Provider stream 入口、C++ stream driver/oracle | 真实有序 stream 和唯一 final；错 generation、gap/timeout、重复/晚到事件按契约拒绝或收束；不能用直接 callback 注入替代线传输 |
| R11-B4 Native Continuation Process | R11-B3 | CLI conversation config、NativeConversationCoordinator、receipt/control/journal、C++ driver | 两轮 FULL_CONTEXT→APPEND_DELTA 经真实 receipt/COMMIT/FINALIZE；冻结 transcript/lineage；错 parent 被拒绝，不将 journal 存在当成功 |
| R11-B5 Native Recovery Process | R11-B4 | requester/journal/Provider state recovery 与 C++ kill/restart driver | 冻结支持的中断点，重启后安全恢复或明确拒绝；不得重复提交 prefix 或把 requester journal 当作 Provider KV 可用证明 |
| R11-B6 Native Replacement Process | R11-B5 | 原生重新规划、fencing、role map/receipt 与第二个 Provider | 真正切换独立备用 Provider，successor 结果/状态正确；旧 attempt/旧 Provider 晚到数据不能提交，无候选时单一失败终态 |
| R11-B7 Native Cleanup Process | R11-B6 | NativeInferenceClient/ProviderHost close、secret lease、Face/io_context/worker 生命周期与 C++ process tests | 成功、取消、超时、失败/替换后清理与排空；共享第二服务仍可用，无残留进程/回调悬空；C++ fixture 显式拥有依赖或 join/drain，不改生产语义掩盖测试竞态 |
| R11-B8 Maintained Callers | R11-B7；对应 T012 原生 ABI | 原 16 caller 清单、同库 facade、兼容入口和 T013 legacy 清退 | 按 caller group 分成有独立出口的子批次；先对照已通过 C++ 行为，再做 wrapper 与真实入口检查；逐项记录 native entry、结果/错误/取消及旧路径零使用；计数本身不关闭 T013 |
| R11-B8-G1 Native Generic Request Facade | R11-B7；T012-A/B | `APPClient.request_task` 与 public `InferenceClient.request_task` 的 generic inline/`REPO_REF` native compatibility route | C++ native owner 回归、模型/task/schema/options identity、结果句柄与 planner non-fallback 通过；stream/conversation、其余 15 callers、legacy zero-use 和资格仍留父卡 |
| R11-B8-G2 Native Generic Stream Facade | R11-B8-G1；R11-B3 stream contract | canonical `APPClient.request_streaming` 的 native stream compatibility route、回调/终态句柄 | C++ native owner 保持 stream 状态和结果权威；Python 只转换 bounded stream options、转发事件/完成/错误回调并保留取消/结果句柄；Python conversation、TOKEN_STREAMING adapter contract、其余 callers 和 legacy zero-use 仍留父卡 |
| R11-B8-G3 Native Dynamic Conversation Binding | R11-B8-G2；R11-B4 continuation contract | C++ `NativeConversationCoordinator`、request planner 与 native client 的首轮 FULL_CONTEXT placement binding | 首轮允许 operator continuation 不携带尚未知晓的 role/provider map；ACK 后由 C++ coordinator 原子绑定 canonical role-map digest 与角色集合，再提交同一 sealed plan；错误/重复绑定拒绝；APPEND_DELTA 与 replacement 的既有绑定约束保持；maintained caller、Provider KV recovery、legacy zero-use 和资格仍留父卡 |
| R11-B8-G4 Native Checkpoint Handle Export | R11-B8-G3；R11-B4 continuation contract | C++ `NativeInferenceHandle`/`NativeConversationCoordinator` commit boundary、pybind handle facade 与 SDK result handles | 成功完成 conversation turn 后只导出 native coordinator 提交的 authenticated opaque checkpoint wire；ordinary/in-flight/failed/cancelled request 返回 empty；Python 只做 bytes 转发，不复制 transcript/Provider state；APPEND_DELTA caller、15 maintained callers、legacy zero-use 和资格仍留父卡 |
| R11-B8-G5 Native Qwen Conversation Caller | R11-B8-G4；R11-B4 continuation contract | `llm_pipeline/user.py` native-config Qwen generation caller、`NativeConversationContinuation` DTO 和 `BoundedQwenGenerationResult` | FULL_CONTEXT/APPEND_DELTA caller 均进入 C++ owner；APPEND_DELTA 只发送已提交 parent transcript 加当前 delta，排除 generation oracle；成功结果只转发 opaque checkpoint bytes；caller mapping 与 fail-closed checks 通过，真实 Provider 第二轮、unavailable-role control、其余 15 callers、legacy zero-use 和资格仍留父卡 |
| R11-B9 Native Closure | R11-B8 | T014 isolation/manifest、安装依赖闭包、T015/16/17 原有卡 | 本地 no-Python/cold path/动态加载与必要反例，真实输出 hash/source/dependency 对齐；T015 通过后完成 T016，全部必要 PO 才可交付；SIF/Tiger 仍由实验机器负责 |

R11-B2 的 evidence 必须关联同一 request/attempt/plan 的 PID、签名身份、ACK、Selection、
handler 开始/结果及最终 Response，并记录实际二进制/共享库摘要和 oracle；只看到
`NATIVE_REQUEST_SUCCEEDED` 字符串或进程 exit 0 不足以通过。R11-B2 可用最小确定性
原生模型建立传输出口，但不得因此取消 T016 的 YOLO/Qwen/保护路径正式覆盖。
N5 前允许维护必要构建闭包以运行当前 C++ 批次；这不提前宣布全面依赖/no-Python 资格。

## Status Preservation

原 T005-A 的进程内 issuer 测试保留为组件证据，不再代表生产 authority 隔离。
T010/T011/T012/T013 保持现有 PARTIAL；新增 process 门满足后仍需满足父任务全部验收。
已有 caller 修改保留，不回滚、不据 Python 用例数量继续推进迁移关闭。
T015 在 N4 后做必要整体收敛审查；不得把“禁止现在再扫描”误解为取消最后的收敛门。
