# Implementation Plan: Reusable TigerCluster YOLO Distributed Inference

**Branch**: `TigerClusterExperiments` | **Date**: 2026-09-06 | **Spec**: [spec.md](spec.md)
**Status**: IN_PROGRESS / LOCAL_EXACT_SIF_PASS / TIGER_GPU_PENDING

## Summary

复用已有身份/路由/进程管理，以及 ACK-driven YOLO、native Provider 和数值比较器；增加一份严格 profile 和薄 YOLO job 入口，补齐 allocation/GPU/跨节点证据。现有 CPU baseline 不是 GPU launcher，旧 `jobs/spec180/yolo-functional.sbatch` 是单节点；不直接改节点数后宣称可用。

### 2026-09-10 current candidate checkpoint

The current layered candidate is APP v39 over the rebuilt v23 base SIF. Base
source selection includes the Controller NAC-ABE callback fix; GCC9 pybind
compilation is reproducible with bounded `-O0 -g0 -B/usr/bin/` and
`BOOST_PHOENIX_DONT_USE_PREPROCESSED_FILES`, while all builds remain at `-j4`.
The v23 SIF SHA is
`sha256:44b44d564c64387716a17588ea387ee2a255948ee744855291c8e7a0676907b0` and
the APP v49 manifest SHA is
`sha256:4a6c3af0c2cc24620c28519404f1a472074ebf354608372934339ae241942db7`.
The extended negative cutpoint contract is consumed by the native producer,
MiniNDN runner and host validator; the native withholding gate suppresses one
fully identified V3 edge.

Real MiniNDN Y-B/Y-N and the fresh shared exact-SIF local owner run
`tiger-local-cpu-v49-r1` are PASS evidence. The local verdict is
`sha256:d15b41c1e853584de893d996741b31d9ee95b4ec89804e0373ad623486c17ebe`
(`NORMAL_EXPERIMENT_PASS`, two requests, nine edges/request, shape `[1,50,6]`,
maximum absolute error `0.0005340576171875`). The earlier v110 publication
callback failure, packaging/extraction failures, stale NFD selection and the
first remote-root transport failure remain immutable diagnostic records.
Historical Tiger single-node `210340` and first normal two-node `210341` remain
provenance only; fresh v49 GPU submission is in progress and T015/T016 still
require new remote receipts. Full evidence and the order are in [the
checkpoint](evidence/tiger-runtime-checkpoint-20260910.md).

### 2026-09-10 execution checkpoint

The bounded sequence now has a fresh local owner PASS for v49. The earlier v35
single-node and first two-node records remain useful substrate provenance, but
they do not qualify the new v23+v49 composition. The new Tiger run must retain
the allocation, node, GPU UUID, Apptainer version, Slurm terminal state and the
same candidate digest before it can close T013. The registered negative
allocation `210342` remains a retained FAIL: it reached Selection and native
withholding, then lost the User observation at 59.9946 s; the current harness
adds cleanup to the completion barrier and the native producer binds one logical
edge. T015 must still be run on a fresh two-node allocation before T016.

## Technical Context

- Language/Version：宿主 Python 3.8+ 兼容；容器 CPython 3.10 由现有交付锁确定，C++/Waf/CMake/bash 沿用仓库。
- Dependencies：四库精确版本取 `development-handoff.lock.json`；base 提供 NDN-CXX/NFD/ORT。补记录包/编译器/Boost/ORT/CUDA 实际版本，不从旧 chat 猜测。
- Host MiniNDN：复用本机 systemd system manager/cgroup transient service，需 root 或 sudo -n；runtime/host_minindn.py 保留独占 unit 身份、时限和整树清理证据。此宿主适配不进入 Tiger Slurm 执行路径，cgroup 空置不替代网络/协议验收。
- Storage：源码、profile/小型证据跟踪；SIF/模型内容寻址缓存，run 输出隔离，秘密为 run 私有 0700 路径。
- Testing：pytest 配置/launcher mutation，Boost 单元/真实集成，MiniNDN，最终 SIF 本地 CPU 和 Slurm GPU。
- Target：两节点、每节点一 GPU、四个独立 Provider；不做 Qwen/扩展性能矩阵。
- Constraints：最多 `-j4`，同构建树串行；本地匹配 base 的容器/SDK 构建；基础 SIF + 外部只读 app→Tiger verify/run；无隐式 CPU fallback；禁止 Spec182 迁移。
- Performance goals：本 Spec 无速度优越性门槛。记录冷启动、warmup、每请求耗时及失败，主要判据为正确性和复用。

## Constitution Check

I/II：沿用动态 API 和现有鉴权/请求级密钥，不新建框架协议；III：CodeGraph 后核对源码；IV/VI：Spec 驱动、每任务一项行为与其回归；V/VIII：T007 收敛后才正式 unit→integration→MiniNDN，后续变更重审；VII：阶段化不可变输入、mutation 零副作用、受控发布、独立终态。
当前设计不修改 Core 安全模型。若发现必要 Core/DI 缺陷，在所属模块做最小完整修复并补 focused regression、更新候选和 T007；不能在 Tiger wrapper 隐藏另一套权限/规划算法。

## Architecture And Ownership

| Owner/path | Existing or planned | Responsibility |
| --- | --- | --- |
| `Experiments/TigerCluster/runtime/baseline.py`, `identities.py` | existing; narrow extension | 公共进程/容器/身份/路由原语；保持已有 CPU v1 schema 及历史结果语义 |
| `runtime/yolo_profile.py` | implemented; qualification open | I/R/E、effective profile、case/run与冻结bundle已接；版本已由issuer/rank/collector消费，剩host语义gate见T007 N1 |
| `runtime/yolo_submission.py` | partial implementation | 共享根下candidate/gate提交状态和未知job恢复；只管理记录，不执行Slurm或验证模型 |
| `runtime/host_minindn.py`, `tools/spec183_minindn.py` | host process owner implemented; three-case qualification pending | 准备输入绑定、systemd runtime/stop 时限、unit身份及cgroup观测；复用有限进程client，不重新实现Core或MiniNDN应用 |
| `runtime/yolo_bundle.py` | integrity implemented; production bundle pending | 显式小型脚本清单冻结/验证，dispatch已调用；不含模型/私钥/宿主库，不替代源/运行资格 |
| `runtime/yolo_worker.py`, `yolo_result.py` | implemented; runtime unqualified | 共享生命周期、四角色和normal/negative留存collector已接；每rank先有界检查版本，public重算要求issuer及所有rank原记录 |
| `apps/yolo.py` | implemented; runtime unqualified | 复用ACK-driven User/签发/准备，per-request独立graph reference已接；不另建模型规划或密钥owner |
| `jobs/yolo/submit.py`, `run.sbatch` | implemented; runtime unqualified | 五命令、normal local/single/two、negative双rank、SSH接收/submit/query和终态已接；真实前置资格仍缺，见tasks.md |
| `profiles/yolo-two-node.json`, `schemas/tiger-yolo-v1.schema.json` | implemented; v48 runtime profile snapshot for current candidate | 一份操作者配置及验证格式；v48 绑定 APP v39/v22 base 与 v40 host receipt；formal local、负例与复用资格仍开放 |
| `adapters/slurm-apptainer/scripts/build-local-sif.sh`, `prepare-development-handoff.py` | existing | 原构建/打包入口，不新增另一个构建器 |
| `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py`, `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` | existing | 当前实际应用/本地网络路径，适配层传参数而不复制 |
| `NDNSF-DistributedInference`, `ndn-service-framework`, dependency repos | existing | DI 计划/执行、NDN 安全/传输、库 ABI；修复归原 owner |
| `Experiments/TigerCluster/tests`, `docs/yolo-reusable.md` | implemented; final acceptance open | 独立实验测试/操作说明已存在；最终成功命令和交付须依真实资格证据校对 |

表中省略前缀的 Tiger 路径均相对 `Experiments/TigerCluster/`。

## Gate Order

2026-09-08 用户授权在已完成NDN小例子基础上推进YOLO真机推理并修复问题。
先做相同 canonical graph/weights/fixture/oracle 的独立CPU→单GPU参考诊断，
1 warmup + 1 measured，固定rtx_6000、五分钟上限。此诊断仅依赖自身脚本/模型/
容器闭包审计与本地CPU实跑，不借用NDNSF尚未完成的门，也不返回NDNSF-DI资格。
然后继续分层应用及安全NDN推理链路；正式四Provider实验仍遵守下方原门禁。

2026-09-08 用户确认采用[稳定基础 SIF + 外置应用](../../Experiments/TigerCluster/docs/runtime-app-layers.md)。
状态 ACCEPTED / IMPLEMENTATION_PENDING；以下分层要求替代“应用全烘焙入SIF”目标。
基础库、通用 Python 绑定、NFD/依赖随 base 固定；DI/UAV 应用及自有扩展独立构建。
当前 builder 九产物安装/preflight 与外部 bundle 校验尚未迁移，T002/T004/T011
必须接线后由 T007 复审，不能将这次文档修改或 NDN 小例子记为 DI 分层资格。

2026-09-08 用户明确要求先做简单 C++ NDN 多节点 SIF 实跑。允许独立的
CPU-only 两节点 transport diagnostic 先于剩余 YOLO 门禁执行，范围仅三条
Interest/Data、容器/NFD/TCP/清理；见 evidence/cpp-ndn-smoke.md。使用已存在
历史 SIF 并在两 rank 校验摘要，不晋升镜像、不计作 GPU/YOLO PASS。

T001已完成接收清点（见evidence/input-inventory.md），发现两个必须在T007前闭合的实际接口差异：现有builder绑定Spec175 tiny-onnx门，T002须扩展原owner接受严格Spec183 YOLO receipt且保留旧行为；正常ACK-driven User为一次请求入口，T005按请求分别调用、独立证据，不用legacy sequential参数假定实现批量。缺物理输入保持WAITING_EXTERNAL_INPUT，不妨碍离线focused实现。

1. G0 / T001：当前源码/交付锁/接口/输入清点。未知环境值列清单；不启动模型或下载大 artifact。
2. G1 / T002–T006：实现配置闭包、launcher/生命周期、YOLO 适配/collector 及 focused 红绿回归。可做小型合成 child-process 测试。
   内部先完成T002内容完整性接口，再T003、T004配置/冻结/提交状态接口、T005/T006，回填T004实际五命令和T002真实命令边界及builder receipt dispatch测试。T004最终可执行命令需要T005应用和T006 collector，不能要求这些消费者存在前关闭T004；T002最终验收同样依赖T004/T006。所有任务仍须在G2前关闭，测试fixture不能冒充T010真实host receipt。
3. G2 / T007：实现到生产调用路径收敛审计，必须 PASS。检查实际 argv/env、角色路由、secure grant/selection、harness/oracle、清理、数据路径。未接线不能算实现。
4. G3 / T008–T010：在本机匹配的基础容器/SDK 中按锁构建或复用闭包（`NAC-ABE + NDN-SVS → NDNSD → NDNSF/Repo及通用绑定 → 外部Apps`），相关unit→真实集成→CPU MiniNDN。不先重复编译一套主机 ORT 版本的应用。构建键未变时只增量编译受影响 app；ABI变更清理消费者。基础构建验收可复用，MiniNDN receipt 绑定实际 source/base/app 组合，用于后续运行资格，不是基础构建前置。
5. G4 / T011：通过原构建 owner 下的分层入口构建或复用基础 SIF，在匹配容器/SDK 生成独立 app 包；DSO/import/help 是启动前检查，合格 MiniNDN gate 后完成精确 base+app 的本地 YOLO 资格。两层产物清单替代“九产物都在SIF”检查。基础/SDK构建可先独立推进；不得将其成功等同 G2 或完整组合资格。单阶段基础镜像保留稳定开发工具可同时作为本地SDK，不要求为同一ABI再生成一份SDK镜像。
6. G5 / T012–T014：目标 compute 环境匹配→精确 SIF 上传/staging 校验→一节点 GPU 四 Provider→两节点 GPU 第一次正常运行（`210340`/`210341` 已闭合）。
7. G6 / T015–T017：先修复并通过一次远端负例（`210342` 暴露 completion-budget 与逻辑 edge cardinality 缺陷），再做第二个独立双节点正常 allocation，最后离线重算和可复用交付；负例未 PASS 前不得启动 T016。

Apptainer 版本探测可能在 T011 前必要。只允许先通过 G2 且 probe 自身的输入/脚本检查，再做有界 substrate allocation；其独立证据不是模型资格，不能要求不存在的 SIF。这消除“先有镜像才能获取构建器版本”的循环依赖。无目标权限/配额则记录 WAITING_EXTERNAL_INPUT，保留可做的本地工作。

## Placement And Data Flow

正式放置由 [profile contract](contracts/experiment-profile.md) 冻结：A 上 Controller/User/Repo 发布方/BackboneNeck/Merge，B 上 DetectShard0/1；A/B 各一 NFD。四 Provider 的证书、PIB/TPM、cache 和 role ID 均独立。两个 DetectShard 在 B 的同一 GPU 上工作不等于两 GPU 并行。
单节点诊断把相同角色投影到一个节点并显式标记 SINGLE_NODE_GPU_PASS；本地 CPU case 标记 LOCAL_CPU_PASS。两个模式不得伪造两个实际 hostname。
当前 YOLO publisher 经 NDN 发布加密 graph/weights/root，native Provider 在角色
cache 内 materialize/assemble；不新增人工 role-only model projection 或 `/artifacts`
Provider mount。发布方的 canonical package 与通用脚本 bundle 分离，Provider 不可
见 oracle。T004只准备代码/配置和发布方输入位置，T005实际连接既有发布/获取 owner；
这是源代码核对后的计划修正，见evidence/native-model-route.md。
图结构和输入生产仍由现有 ACK-driven DI 路径决定；job 层约束允许候选/放置，不自行伪造 ACK 或跳过 plan sealing。下游只依赖真实选定/本地模型 ready/直接前驱 Data，不增加全局“所有阶段就绪后才能发数据”的屏障。

## Candidate And Change Invalidation

运输采用[同绝对路径布局](contracts/experiment-profile.md#cross-host-transport-layout-design-fixed-transport-not-implemented)：在本机project镜像目录完成新的本地资格，随后原字节复制至Tiger同名目录。旧回执不改路径或candidate；本地同名目录不是Tiger身份证明。运输清单、互斥锁、接收校验、SSH断点续传与唯一submit/query已接（T004.n–q）；登录节点小文件验证不等于完整候选运输或GPU资格。当前控制缺口见[生产审计N1–N3](evidence/design-code-convergence.md)，不重复实施运输。

使用 [candidate contract](contracts/experiment-profile.md#candidate-identity) 的 input/runtime/experiment 三阶段身份；每次 final candidate 关联同一输入身份。构建输入清单和运行资格清单用途不同，不人工填 PASS。

Spec183 的 R 只固定基础运行库；E 包含只读 appManifest 与 harness。应用包可以
包含 DI 自有原生产物，必须在匹配 base 的容器/SDK 编译并验证 ABI，不能夹带
宿主依赖或遮蔽基础库。`447f7584`保留为旧交付来源；DI/app 修复改变 appManifest/E，
基础 Core/Repo/通用绑定修复才改变 I/R 并重建消费者。分层清单显式版本化，旧
R/E 回执不重解释。混合仓库按实际基础源码闭包 seal，不能用 app commit 强迫
重建未变基础库，也不能遗漏基础构建输入。禁止覆盖 SIF 内 native/runtime 路径。

| Changed plane | Earliest restart | Evidence retained/reused |
| --- | --- | --- |
| 纯文档、run ID、等价 artifact 物理位置 | schema/path/hash 检查 | 不重编译；新 run identity，旧执行记录保留 |
| Core/Repo通用库或绑定、基础dependency/toolchain/build definition | T007 → T008 | 新base，干净重建受影响 ABI consumers，再验证精确组合 |
| DI/UAV app C++或自有扩展 | T007、相关T008–T011 | 只增量构建受影响app目标及消费者；base不变，验证app ABI/行为 |
| app Python entrypoint/打包内容 | T007、相关import/行为门 | 冻结新app/E，不重编C++或重建SIF；网络/安全变更补对应门 |
| SIF bytes | T007 → T011，基础源码变化则 T008 | 新R身份，验证对应app组合；不可复用旧E执行PASS |
| harness/launcher/身份或路由/effective config | T007，相关 focused/unit/integration/MiniNDN | 无 native 变化可复用 SIF；不能复用旧正式运行 |
| 模型/输入/预处理/oracle/tolerance | T001 → T007、相关本地数值/网络 gates | 无 ABI 变化不重建 SIF；重新注册 workload 后再运行 |
| host/GPU/driver/Apptainer/scratch | T012 allocation preflight | 文件身份可复用；新节点的环境必须实测 |

行为变化必须更新 spec/contract 及 affected gates；原失败记录不可覆盖。phase-reuse receipt 列出未变的哈希及为何可复用。

## Verification And Resource Boundaries

全部场景与任务映射见 [validation matrix](validation-matrix.md)。旧 r119 的预期 abort134 不直接成为新版本的负例标准，T001 从当前实现核对错误合同。模型 GPU 正例须 observed CUDA EP/执行记录；可见 GPU 不足，Merge CPU 后处理按角色单列。
每 job 的资源、timeout、请求数固定；不后台启动无限 campaign。容量检查用 SIF/模型/中间构建峰值加登记余量；缓存命中不重复下载/拷贝。阶段开始检查和失败时复查即可，清理只处理清单内归本 run 的临时内容。

## Project Structure

设计、契约、任务、审计在本 Spec；实际运行脚本/配置/测试/操作说明在 `Experiments/TigerCluster`；通用实现和测试保留原 owner。不移动冻结历史脚本，不把旧兼容 symlink 改成复制品。

## Migration And Rollback

保留 `tiger-two-node-v1` 和 baseline/service-echo 调用契约；新增 YOLO schema 单独 dispatch 到共享原语，旧 profile 不自动升级。移除新 YOLO consumer 不影响旧入口。失败 candidate 不能替换已知合格 SIF；回退只选择已有完整匹配的候选，不拼接旧库/新扩展。
本机 `.planning/STATE.md` 的旧 phase35/Spec168 不代表本任务进度。`tasks.md` 为 Spec183 权威，`.planning/spec183-handoff.md` 只记录入口和下一项任务，不恢复旧 Qwen campaign。

## Complexity Tracking

新增的是一个实验 consumer 和其严格配置/判定，不新增调度平台、镜像工厂、通用数据库或权限协议。共享封装只在已有 baseline 与 YOLO 都调用时抽取。可复用并不意味着承诺所有未来模型无需验证。
# Local tool binding clarification (2026-09-08)

The actual host uses Apptainer1.5.3 at/opt/apptainer/1.5.3/bin/apptainer;
Tiger's declared environment uses1.3.4-1.el9 at/usr/bin/apptainer. Keep the
same base/app composition and declare local tools explicitly in runtime.local.
Local-cpu issuer/ranks and their public reanalysis use the local declaration;
cluster cases retain the cluster declaration. Both versions remain frozen in
effective profile behavior, allowing gate reuse to compare the same complete
profile while each environment supplies its own actual observation.
Reuse tools/spec183_dev_provision.py for signed development preparation; its
prepared-run validation must use the canonical decoder and it must reject a
CLI executable that differs from the selected declaration.

## 2026-09-10 operator sequence and failure rule

For a local candidate, run `submit.py prepare` and then `submit.py local` on
that prepared run. The local command owns issuer preparation, process startup,
collection and cleanup; calling the development provision helper or a direct
executor after it has created an execution record is a `LOCAL_RUN_ALREADY_STARTED`
failure. Every partial attempt receives a new run ID.

For a GPU candidate, `submit.py submit` must use the refreshed host/local gate
and the exact shared base+APP composition. A CUDA Provider is not selectable
until its signed V3 ACK contains a resource row for the advertised device with
enough free memory. Empty rows, stale rows or a missing CUDA measurement stop
at placement and remain a retained diagnostic failure. The first acceptable
GPU evidence therefore includes ACK/Selection, warmup and measured numerical
responses, cross-role data, backend/GPU records and complete cleanup.
