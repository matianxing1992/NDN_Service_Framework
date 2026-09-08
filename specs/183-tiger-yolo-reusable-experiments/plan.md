# Implementation Plan: Reusable TigerCluster YOLO Distributed Inference

**Branch**: `TigerClusterExperiments` | **Date**: 2026-09-06 | **Spec**: [spec.md](spec.md)
**Status**: IN_PROGRESS / runtime NOT_RUN

## Summary

复用已有身份/路由/进程管理，以及 ACK-driven YOLO、native Provider 和数值比较器；增加一份严格 profile 和薄 YOLO job 入口，补齐 allocation/GPU/跨节点证据。现有 CPU baseline 不是 GPU launcher，旧 `jobs/spec180/yolo-functional.sbatch` 是单节点；不直接改节点数后宣称可用。

## Technical Context

- Language/Version：宿主 Python 3.8+ 兼容；容器 CPython 3.10 由现有交付锁确定，C++/Waf/CMake/bash 沿用仓库。
- Dependencies：四库精确版本取 `development-handoff.lock.json`；base 提供 NDN-CXX/NFD/ORT。补记录包/编译器/Boost/ORT/CUDA 实际版本，不从旧 chat 猜测。
- Storage：源码、profile/小型证据跟踪；SIF/模型内容寻址缓存，run 输出隔离，秘密为 run 私有 0700 路径。
- Testing：pytest 配置/launcher mutation，Boost 单元/真实集成，MiniNDN，最终 SIF 本地 CPU 和 Slurm GPU。
- Target：两节点、每节点一 GPU、四个独立 Provider；不做 Qwen/扩展性能矩阵。
- Constraints：最多 `-j2`，同构建树串行；Container native build；本地 SIF→Tiger verify/run；无隐式 CPU fallback；禁止 Spec182 迁移。
- Performance goals：本 Spec 无速度优越性门槛。记录冷启动、warmup、每请求耗时及失败，主要判据为正确性和复用。

## Constitution Check

I/II：沿用动态 API 和现有鉴权/请求级密钥，不新建框架协议；III：CodeGraph 后核对源码；IV/VI：Spec 驱动、每任务一项行为与其回归；V/VIII：T007 收敛后才正式 unit→integration→MiniNDN，后续变更重审；VII：阶段化不可变输入、mutation 零副作用、受控发布、独立终态。
当前设计不修改 Core 安全模型。若发现必要 Core/DI 缺陷，在所属模块做最小完整修复并补 focused regression、更新候选和 T007；不能在 Tiger wrapper 隐藏另一套权限/规划算法。

## Architecture And Ownership

| Owner/path | Existing or planned | Responsibility |
| --- | --- | --- |
| `Experiments/TigerCluster/runtime/baseline.py`, `identities.py` | existing; narrow extension | 公共进程/容器/身份/路由原语；保持已有 CPU v1 schema 及历史结果语义 |
| `runtime/yolo_profile.py` | implemented; qualification open | I/R/E、effective profile、确定性case/run与冻结bundle已接；host语义gate和版本消费缺口见T007 N1/N3 |
| `runtime/yolo_submission.py` | partial implementation | 共享根下candidate/gate提交状态和未知job恢复；只管理记录，不执行Slurm或验证模型 |
| `runtime/yolo_bundle.py` | integrity implemented; production bundle pending | 显式小型脚本清单冻结/验证，dispatch已调用；不含模型/私钥/宿主库，不替代源/运行资格 |
| `runtime/yolo_worker.py`, `yolo_result.py` | implemented; runtime unqualified | 共享生命周期、四角色和normal/negative留存DAG/GPU/数值/清理collector已接；每rank版本检查待补 |
| `apps/yolo.py` | implemented; runtime unqualified | 复用ACK-driven User/签发/准备，per-request独立graph reference已接；不另建模型规划或密钥owner |
| `jobs/yolo/submit.py`, `run.sbatch` | implemented; runtime unqualified | 五命令、normal local/single/two、negative双rank、SSH接收/submit/query和终态已接；真实前置资格仍缺，见tasks.md |
| `profiles/yolo-two-node.json`, `schemas/tiger-yolo-v1.schema.json` | implemented; candidate refresh pending | 一份操作者配置及验证格式；图/模型等外部输入仅以immutable引用出现；最终source/R/E尚未资格化 |
| `adapters/slurm-apptainer/scripts/build-local-sif.sh`, `prepare-development-handoff.py` | existing | 原构建/打包入口，不新增另一个构建器 |
| `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py`, `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` | existing | 当前实际应用/本地网络路径，适配层传参数而不复制 |
| `NDNSF-DistributedInference`, `ndn-service-framework`, dependency repos | existing | DI 计划/执行、NDN 安全/传输、库 ABI；修复归原 owner |
| `Experiments/TigerCluster/tests`, `docs/yolo-reusable.md` | implemented; final acceptance open | 独立实验测试/操作说明已存在；最终成功命令和交付须依真实资格证据校对 |

表中省略前缀的 Tiger 路径均相对 `Experiments/TigerCluster/`。

## Gate Order

T001已完成接收清点（见evidence/input-inventory.md），发现两个必须在T007前闭合的实际接口差异：现有builder绑定Spec175 tiny-onnx门，T002须扩展原owner接受严格Spec183 YOLO receipt且保留旧行为；正常ACK-driven User为一次请求入口，T005按请求分别调用、独立证据，不用legacy sequential参数假定实现批量。缺物理输入保持WAITING_EXTERNAL_INPUT，不妨碍离线focused实现。

1. G0 / T001：当前源码/交付锁/接口/输入清点。未知环境值列清单；不启动模型或下载大 artifact。
2. G1 / T002–T006：实现配置闭包、launcher/生命周期、YOLO 适配/collector 及 focused 红绿回归。可做小型合成 child-process 测试。
   内部先完成T002内容完整性接口，再T003、T004配置/冻结/提交状态接口、T005/T006，回填T004实际五命令和T002真实命令边界及builder receipt dispatch测试。T004最终可执行命令需要T005应用和T006 collector，不能要求这些消费者存在前关闭T004；T002最终验收同样依赖T004/T006。所有任务仍须在G2前关闭，测试fixture不能冒充T010真实host receipt。
3. G2 / T007：实现到生产调用路径收敛审计，必须 PASS。检查实际 argv/env、角色路由、secure grant/selection、harness/oracle、清理、数据路径。未接线不能算实现。
4. G3 / T008–T010：按锁干净构建（`NAC-ABE + NDN-SVS → NDNSD → NDNSF → Apps/两个 Python 扩展`），unit→真实集成→CPU 小模型 MiniNDN。宿主库路径和编译/链接工具闭包要实测。
5. G4 / T011：合格 host-gate manifest 后通过原入口本地构建完整 SIF，容器内九原生产物/所有 DSO/import/help/CPU 小模型验证。
6. G5 / T012–T014：目标 compute 环境匹配→精确 SIF 上传/staging 校验→一节点 GPU 四 Provider→两节点 GPU 第一次正常运行。
7. G6 / T015–T017：小规模负例→第二个独立双节点正常 allocation→离线重算和可复用交付。

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

Spec183 外部 Python job/应用适配器可以作为 E 中的只读脚本 bundle 挂载到固定 R；须通过该 R 的实际已安装 API/import 检查，不能夹带宿主 `.so`。`447f7584`只保留为旧交付来源；后续User/DI/native cutpoint修复必须纳入新的I/source lock与R，不能把旧运行库身份继续称为当前候选。镜像内应用或Core/DI变化须先重seal并构建R，再验证E；不能在旧SIF上临时覆盖native/runtime文件。

| Changed plane | Earliest restart | Evidence retained/reused |
| --- | --- | --- |
| 纯文档、run ID、等价 artifact 物理位置 | schema/path/hash 检查 | 不重编译；新 run identity，旧执行记录保留 |
| Core/DI/dependency/toolchain/native build definition | T007 → T008 | 旧运行标历史，干净重建 ABI consumers，再 SIF/下游 |
| Python entrypoint/打包内容、SIF bytes | T007 → T011；若宿主同路径变化则 T008 | 只重跑受影响本地 gates，但最终新 SIF 必须重新验证 |
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
