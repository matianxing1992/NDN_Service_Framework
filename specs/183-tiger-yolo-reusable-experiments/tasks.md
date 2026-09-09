# Tasks: Reusable TigerCluster YOLO Distributed Inference

**Input**: [spec.md](spec.md), [plan.md](plan.md), [profile contract](contracts/experiment-profile.md), [validation matrix](validation-matrix.md)
**Branch**: `TigerClusterExperiments`
**Status**: 2/17 parent tasks complete (T001 inventory and T003 focused component acceptance); IN_PROGRESS. Exact-SIF MiniNDN v48 normal Y-B and v49 registered Y-N now have local runtime PASS records, but no formal T010 qualification or Tiger GPU PASS. The shared host-gate semantic validator and three-case producer are implemented and focused-tested; the required post-Selection dependency run is still missing. Standalone C++ NDN/SIF diagnostic passed in Tiger job 209981.

## Detailed Execution Progress

当前检查点：[分层本地启动实证](evidence/layered-local-startup.md)。修复后的基础
SIF与外置DI应用已通过精确组合闭包；v13 local-cpu 在持久Provider进程中完成
warmup+measured 两次请求，均返回数值结果并通过 oracle 比较。该证据仍限于本机
CPU 执行，不能关闭 MiniNDN、GPU 或 TigerCluster 资格门。

新增检查点：[v25 exact-SIF MiniNDN Y-B](evidence/minindn-v25-exact-sif-yb.md)。
同一基础 SIF 与外置应用在真实 MiniNDN 进程边界完成正常 Y-B：T010_DONE、四 ACK、
selection、protected grant、terminal response、数值 oracle 与 clean cleanup 均有
回执。该结果确认正常 APP/NDNSF-DI 数据路径已运行；Y-A/Y-N、host qualification
manifest、GPU 与 TigerCluster 资格仍未完成。

新增检查点：`minindn-local-20260909-v40-yn36` exact-SIF Y-N 矩阵。使用同一 v21
基础 SIF 与 v24 外置应用，Y-N-O/C/P/R/I/E/L 七个注册子案全部 `status=PASS`；
正常控制、选择/ACK/角色拒绝、非 ingress 输入拒绝、三种 grant 拒绝和 redaction
拒绝均有独立回执。systemd supervisor `returncode=0`、`processCleanup=CLEAN`，
九个 Controller 日志无 traceback/abort。该回执仍是 `qualification=NOT_EVALUATED`
的本地 MiniNDN 证据，不能关闭 Y-A、同源 host qualification manifest、GPU 或
TigerCluster 资格门。

新增检查点：[v47 exact-SIF Y-A](evidence/minindn-v47-exact-sif-ya.md)。v45 已证明
SIF、ACK/Selection、输入取回和真实 ORT CPU 执行均正常，但原子 FullModel 将
`[1,300,6]` 原始预测直接作为终端响应，触发响应大小/保密性边界；随后修复了
`ONNX_POSTPROCESS` 终端契约，并修复 APP candidate 构造中的自引用
`UnboundLocalError`。v47 使用同一基础 SIF 与重建 v31 APP，真实 MiniNDN 返回
`returncode=0`，终端回执 `status=PASS`，数值回执 `shape=[1,50,6]`、`matched=true`、
`maxAbsError=0.0005340576171875`，User 日志为 `YOLO_ACK_DRIVEN_RESULT status=true`。
该结果仍保持 `qualification=NOT_EVALUATED`，只关闭当前本地 Y-A APP 路径缺陷，
不关闭 host manifest、GPU、TigerCluster 或两节点资格门。

2026-09-08 用户裁决将本仓库 TigerCluster 构建并行度上限从 `-j2` 调整为 `-j4`，
用于缩短构建时间并保持可复现；同一构建树仍只允许一个构建进程。历史回执保留实际
使用的 `-j2` 命令，不改写为新规则下的执行证据。

| 细分任务 | 状态 | 实证 / 下一步 |
|---|---|---|
| T004.local-tools：本机与Tiger工具版本绑定 | IMPLEMENTED | 本机1.5.3签名准备成功；Tiger版本要求保留 |
| T009.prepare：实际分层候选签名准备 | EXECUTED | layered-host-20260908a，receipt46405701；非推理PASS |
| T009.role-home：角色身份挂载修复 | VERIFIED_STARTUP | 同SIF内身份可见、根身份隔离；Controller签名发布成功并清理 |
| T009.cpu-chain：完整CPU链 | VERIFIED_LOCAL_CPU | v13 新基础+外置应用；warmup 与 measured 两请求均 `REQUEST_ACCEPTED`、`YOLO_ACK_DRIVEN_RESULT status=true`，每次 1267 bytes，`matched=true`，`maxAbsError=0.0005340576171875`；仍需 MiniNDN/单节点GPU/双节点GPU |
| T009.controller-version：协作请求版本绑定 | SOURCE_VERIFIED | 原SIF两入口均缺版本，exit201；修复ServiceUser单对象后版本绑定及撤权拒绝断言exit0；待新原生基础层和应用消费者验证，Provider检查不变 |
| T009.native-refresh：更新修复后的基础层 | VERIFIED_COMPOSITION | commit `2ffa36b7` 的 request-owned artifact 修复已编入 base v6；SIF `sha256:d6aab730…0b01d`，`BASE_LIBRARIES_ONLY` PASS；app v13 manifest/buildKey `58b54e2d…db013`，Python import 与三入口 `ldd -r` 均 PASS |
| T009.repo-order：Repo双Face管理命令顺序 | VERIFIED_STARTUP | 修复已装入ccdd4ac0基础SIF；候选c越过原注册故障；48项HA及新增失败清理检查沿用，STATUS响应仍待修复 |
| T009.acceptance：开发入口请求结果验收 | IMPLEMENTED | 使用生产graph-reference/result校验后才记录accepted；保留结果目录；3项定向检查通过，不代表实际推理通过 |
| T009.repo-ack：Repo保护模式ACK适配 | SOURCE_VERIFIED | 保护模式改为能力ACK、保留Selection后操作/身份校验；最小4进程诊断STATUS首次请求READY、User0、全清理；显式源码挂载，未封装或验收完整YOLO |
| T009.repo-select：隐藏输入下对象定位 | IMPLEMENTED | manifest在总deadline内逐一尝试未知位置Provider，响应确认对象；delete使用确认副本；53项HA检查分两次完成（52通过、1新断言修正后通过）；待完整新组合运行 |
| T009.fixture：外置应用固定输入资产 | VERIFIED_INPUT | app71aecff6只新增已封存固定图片，原159文件不变、buildInvoked=false；真实User生成[1,3,640,640]输入，未运行模型；完整候选待验 |
| T009.request-id：冻结V2规范请求ID | SOURCE_FIXED | e实证多组件计划ID被DI转换为单组件URI，日志严格拒绝；计划生成器提前编码，定向回归先红后绿；不改一致性检查，不重打SIF/app |
| T011.protected-base：封装完整Repo ACK修复 | VERIFIED_COMPOSITION | 新SIF c6dbeda8独立验证6原生产物/NCCL/Repo源码；app38289c5f全部159文件不变，configure5.629s/Waf0.851s无C++编译；导入、User入口、3二进制闭包通过；完整CPU运行待验 |
| T011.base-python：封装修正后的Repo库 | VERIFIED_COMPOSITION | [重封装](evidence/base-python-repack.md)：新SIF ccdd4ac0独立验证；6原生产物不变；app89c49f7a全部159文件不变，configure7.409s/Waf1.036s无C++编译；组合闭包/入口通过，完整运行待验 |

用户指定的 [C++ NDN/SIF 两节点小例子](evidence/cpp-ndn-smoke.md)已实跑通过：
209981，itiger01/02，三次 Interest/Data，Slurm 0:0，清理完成。
2026-09-08 用户进一步确认“基础库SIF + 外置DI/UAV应用”方案：
[runtime layers](../../Experiments/TigerCluster/docs/runtime-app-layers.md)，设计已记录，代码迁移未完成。
下一步先接T002/T004/T011分层清单、ABI及构建/挂载，再复用这套配置推进最小NDNSF服务与YOLO。
同模型独立CPU与GPU参考已完成：[backend reference](evidence/yolo-backend-reference.md)。
209982因默认TF32数值失败；仅关闭TF32后的209983在RTX6000Ada上实跑PASS，
既定容差不变。精度修复已同步native/reference源码，正式NDNSF-DI四Provider仍未通过。
原 17 项验收及既有 MiniNDN 交付证据不被替换。

更新：2026-09-08；进度表初始审计基线 `d1f1504a`，`cc638d00`审计后N3源码修复，见
[runtime version](evidence/t004-runtime-version.md)；[生产审计](evidence/design-code-convergence.md)仍BLOCK于N1/N2。其他客户端已在
`64df1581` / `5171450d` 提交 [host-unit](evidence/host-unit.md) 的构建与加载证据；
下表记录其已声明范围，未重复运行构建，也不将它升级为正式 runtime 资格。
下表是当前执行入口；后文 checkpoint 是历史证据，不应把旧“下一步”当作当前指令。
`VERIFIED` 仅指该行声明的范围；历史组件测试记录未在本轮重跑，不能证明当前候选
的 runtime PASS。`IMPLEMENTED` 表示代码/脚本存在；`BLOCKED` 表示仍有明确前置缺口。
子步骤不增加顶层任务数，不按行数计算完成百分比。父任务仍须满足原验收条件才勾选。

### T001–T007：关闭实际 GPU YOLO 执行路径

T007 复核确认正常/负例/SSH 接线已存在；N3 issuer/rank版本检查及collection/运输已修，
当前控制项为 N1 host gate 语义重算、N2 MiniNDN 三场景及有界准备/执行。
旧“运输/负例/certified graph 未接”的历史状态不再适用。T010/T011 实际回执是
后续验收，不倒置为 T007 的物理前置；完整候选运输、SIF 与 GPU 资格仍未验收。

| Step | Parent | Concrete outcome / path | State | Evidence / verification scope | Blocker / next action | Reuse / rerun trigger |
| --- | --- | --- | --- | --- | --- | --- |
| T001.a | T001 | 四库、模型、工具、资源与接口接收清点 | VERIFIED | [input-inventory](evidence/input-inventory.md)；清点验收，非运行资格 | 新 candidate 更新输入身份；缺项仍显式保留 | 未变输入复用；source/model/tool 变化重核 |
| T002.a | T002 | `runtime/yolo_profile.py` 的 I/R/E 完整性与失效检查 | IMPLEMENTED | [integrity](evidence/t002-integrity.md)；有历史 focused 证据 | 与最终生产入口重核，不能以 helper 关闭 T002 | 只重测变化 plane 及零副作用边界 |
| T002.a2 | T002 | 显式分层I/R/E生成器接独立base/app输入 | VERIFIED | [layered plane render](evidence/layered-plane-render.md)：真实RAM候选通过public check，29harness/159app文件；NOT_EVALUATED、exit78；4focused通过 | 仅内容范围；本机/Tiger版本绑定、实际MiniNDN及N1语义资格尚未完成 | 磁盘缓存单比特差异会复现，保留RAM基础镜像；仅复制小清单，不重建SIF |
| T002.b | T002 | 既有 builder 接受 Spec183 host-gate receipt、保留 Spec175 | IMPLEMENTED | [T010 host-gate producer](evidence/t010-host-gate-producer.md)：v2 validator/producer 对生命周期、数值、角色执行、故障边界和清理做共享语义重算；18 focused tests pass | 真实同源三场景 receipt 仍待生成；当前 Y-N-C placement failure 必须继续拒绝 | 只增加空语义/错误故障/退出清理在真实消费边界的拒绝检查 |
| T003.a | T003 | `yolo_worker.py` 角色隔离、启动、进程组及清理 | VERIFIED | [worker](evidence/t003-worker.md)；T003 focused acceptance | 真实 workload 接线归 T004/T005 | 生命周期代码未变复用；改动时跑对应回归 |
| T004.a | T004 | profile/schema、冻结 bundle、五命令与提交 journal | IMPLEMENTED | [runtime version](evidence/t004-runtime-version.md)：N3已接issuer/rank→public重算→运输；37首组、78修复组、11最终边界通过（有重叠） | N3源码范围已验证；实际版本资格归T011/T012；T004仍依赖N1/N2收敛 | 无native/SIF/模型运行；版本边界不变不重复检查集合，实际新rank仍观测一次 |
| T004.b | T004 | `jobs/yolo/submit.py` 的远端 staging、run 接真实 worker | IMPLEMENTED | T004.k/q/s已连接正常与负例入口；实际提交仍要求前置资格门 | T007统一审查所有有效字段/完整调用链；完整候选前置证据仍缺 | 只验证新调用边界，复用既有 worker/barrier/journal 证据 |
| T004.c | T004 | `local` 拒绝其他 profile 冻结的 prepared run | VERIFIED | [CLI binding](evidence/t004-cli-journal.md#2026-09-07-local-prepared-profile-binding)；40 CLI passed，先于 host receipt 检查拒绝 | 绑定修复已完成；完整调用链等待T007审查 | 相同源码无需重复 CLI suite；本项不触发 SIF/GPU 验证 |
| T004.e | T004 | v2 prepared 绑定 I/R/E；跨运行 prerequisite 重算留存证据 | IMPLEMENTED | [gate reuse](evidence/t004-gate-reuse.md)；56 项 CLI/门边界检查通过，collector double 不代表运行通过 | 运输接线见T004.q；真实先行运行证据仍缺；旧 v1 不放行 | 同内容复用留存证据；改变内容/脚本/行为拒绝复用，不启动模型重复测试 |
| T004.f | T004 | 冻结脚本→刷新文件摘要→有效配置快照；检查当前语义一致性 | VERIFIED | [effective profile](evidence/t004-effective-profile.md)；79 项组件通过，含真实小文件 renderer 一次收敛/重复稳定性 | 仅本行配置一致性通过；T004.s已接，最终候选E待刷新 | 只重测 renderer/profile/prepare/provision；不重跑模型或旧单位套件 |
| T004.g | T004 | 单 GPU batch→srun→journal-bound rank→真实单节点 owner→collector | IMPLEMENTED | [single GPU runner](evidence/t004-single-gpu-runner.md)；63 项首轮、11 项最终边界通过；无真实 Slurm/GPU 执行 | 后续编排已接T004.h–s；候选前置资格与T007仍待闭合；不关闭T013 | 复用 CPU owner 与已有 allocation/GPU probe；仅测新增编排、错误与清理边界 |
| T004.h | T004 | 两 rank 共用签发和nonce、独立allocation/角色、全部退出后汇总 | IMPLEMENTED | [two-node runner](evidence/t004-two-node-runner.md)；41 owner/handoff + 53 CLI 检查通过，83 个唯一用例；无真实 Slurm/GPU | 630秒最小预算、profile900；暂存/scratch/终态已有后续接线；负例与T014仍未验收 | 双 rank 并发组件 + 实际留存文件汇总；未运行额外模型case |
| T004.i | T004 | 实测容量、每 rank 校验复制同一 SIF、node-local NFD、持久清理凭据 | IMPLEMENTED | [node storage](evidence/t004-node-storage.md)；56 + 66 项局部通过；真实小文件/fsync/socket，native/Slurm 编排使用 double | 实际候选刷新两次 FILE_DIGEST:baseSif，独立读取6a3d0010与锁b6710fd6不符；先查读取/内容完整性；T012仍未验收 | 不再盲重试刷新；仅测变化边界，无模型/GPU重跑 |
| T004.j | T004 | 作业外 `collect --reconcile` 查询终态、持久观察、释放journal；GPU前置门要求终态 | IMPLEMENTED | [terminal observer](evidence/t004-terminal-observer.md)；78项首组 + 4项gate + 1项修正fixture通过；真实journal和文件、scheduler double | 后续接线见T004.k/q/s；实际负例验收与 [input read integrity](evidence/input-read-integrity.md) 仍缺 | 默认collect离线；只测新终态/门边界，不启动GPU或复跑旧模型 |
| T004.k | T004 | 共享目录接收端唯一sbatch；intent→SUBMITTING→ack，未知job按comment查询恢复 | IMPLEMENTED | [shared submit](evidence/t004-shared-submit.md)；85首组、15最终接收边界通过；真实journal/files、Slurm double | 环境/运输/负例已接T004.l/q/s；真实候选gate与镜像读取异常未解决 | 只重测提交/journal/ack与新依赖门；未启动GPU、重编native或重复下载SIF |
| T004.l | T004 | 独立operator venv、固定解释器配置贯穿提交/batch/srun，保留本机离线入口 | IMPLEMENTED | [operator environment](evidence/t004-operator-env.md)；实际Tiger安装/复用和本机pins通过；104首组、78最终边界通过 | 登录节点依赖已验证；compute rank、完整候选运输和runtime资格仍未验收；不扩大为SIF/GPU PASS | 复用按依赖摘要命名的环境，不重装系统Python、不改SIF；每实际rank检查加载 |
| T004.m | T004 | sender submit使用本机解释器；两端同名project路径保留验收原字节 | IMPLEMENTED | [submit origin](evidence/t004-submit-origin.md)；68项组件；T004.q已验证小型project镜像与SSH协调 | 完整真实候选运输仍未验收；不以小文件fixture代替SIF资格 | 仅重测提交解释器和接收边界；不修改旧回执、不重跑模型或下载SIF |
| T004.n | T004 | 显式文件inventory与不覆盖接收发布；中断后复用已匹配文件 | IMPLEMENTED | [transport receiver](evidence/t004-transport-receiver.md)；15组件检查通过；真实SSH两文件首次发布/复用通过 | 清单/锁/SSH已接T004.p/o/q；完整候选和计算节点尚未验收 | 只测新增文件/发布边界；11KB工具与合成输入，不传模型/密钥/SIF |
| T004.o | T004 | transport独占namespace锁，journal共享锁；未关闭/未知提交阻止发布 | IMPLEMENTED | [transport guard](evidence/t004-transport-guard.md)；83首组、41最终检查；Tiger登录节点真实双进程互斥通过 | 运输已接T004.q；计算节点/跨节点锁语义未实测；最终候选须冻结新版harness/同一lock root | 无GPU/Slurm，只传16KB工具和synthetic journal；复用现有提交状态机 |
| T004.p | T004 | `submit --plan-transport`经原门验证后枚举candidate与前置验收文件 | IMPLEMENTED | [transport inventory](evidence/t004-transport-inventory.md)；80首组、21入口、9最终清单检查通过；fixture scope | SSH已接T004.q；真实候选未导出；最终harness须更新E | 无模型/SSH/Slurm；按原collector输出取所需文件，不复制角色HOME或重跑模型 |
| T004.q | T004 | 同名project目录的SSH运输、断点续传、接收校验→唯一提交入口 | IMPLEMENTED | [SSH coordinator](evidence/t004-ssh-coordinator.md)；15新边界、82受影响检查通过；真实Tiger登录节点31文件507380字节发布/同run复用成功 | 运输不是GPU资格；28文件harness待冻结；实际负例验收与候选前置门仍未完成 | 仅约507KB运输fixture，无Slurm/SIF/模型；失败记录保留，匹配文件和原journal复用 |
| T004.r | T004 | 原生V3输出在契约校验后、首包发布前阻断；Tiger指定DetectShard0→Merge | IMPLEMENTED | [dependency cutpoint](evidence/t004-dependency-cutpoint.md)；27参数检查、3新原生精确Data集成case、handler/入口语法通过 | T004.s已接User/collector；native源码须重seal/构建并实际验证 | 本轮复用原生证据，未重编或重跑；原生/真实GPU验收留在对应门 |
| T004.s | T004 | 单个负例User终态→双rank调度→留存Selection/故障/清理证据的collector | IMPLEMENTED | [negative collector](evidence/t004-negative-collection.md)；191首组、52入口、12存储/JSON、21响应边界通过（有重叠） | 已移除未接线占位拒绝；前置两节点资格门仍强制；T007审查，T015实际运行未验收 | 共用正常owner/锁/清理；只重测变化边界，无native/SIF/GPU；28文件harness待冻结 |
| T004.d | T004 | local→冻结 CLI→签发→两个 CPU 请求→清理→真实 collector | IMPLEMENTED | [local owner](evidence/t004-local-owner-wiring.md)；114 个唯一组件用例最终有通过记录，非 native/SIF PASS；host seal/九产物、冻结 NumPy owner 已接 | 等 T007 收敛和真实 T010 receipt/新 SIF 后运行，不能以接线关闭 T004 | 首轮仅剩准备 fixture 漂移，修后只重跑该模块12项；不重复全部集合 |
| T005.a | T005 | 真实 User/Provider 参数、权限材料、准备与 readiness | IMPLEMENTED | [public recipients](evidence/t005-public-recipients.md)、[normal owner](evidence/t005-normal-node-owner.md) | 尚未证明完整真实 YOLO request→response | 未变安全组件证据复用；变更只重测影响边界 |
| T005.b | T005 | User post-ACK role specs→DI assembler→独立 ORT reference→发布后 MODELROOT | IMPLEMENTED | [request wiring](evidence/t005-request-reference-wiring.md)；176 组件通过，含真实小 ONNX assembler/CPU ORT；原生应用测试收集失败 | 生产调用已接；需修复 native loader 后验证真实 YOLO request，T007 仍 BLOCK | 组件证据复用；native 测试待 loader 修复后再跑，不为 identity mutation 重建模型 |
| T005.d | T005 | 独立参考区分角色逻辑摘要与组装 ONNX 字节摘要 | VERIFIED | [reference identities](evidence/t005-reference-identities.md)；145 focused passed，CPU ONNX/组件范围 | 后续 producer 传入两个身份，并在发布后绑定实际 MODELROOT manifest；T005.b 未关闭 | 仅重跑 reference/相关 comparator，不触发 native build 或历史 suite |
| T005.c | T005 | 新 SIF 内 Controller 写真实 publication receipt | BLOCKED | [旧 G2 处置](evidence/design-code-convergence.md)；当前host源码支持参数，旧base不支持 | T011新SIF执行验证；不是要求T011先于T007的源码阻塞 | 旧 base 不反复尝试同一不支持参数 |
| T006.a | T006 | `yolo_result.py` 数值、角色、边、GPU、退出与负例判定 | IMPLEMENTED | [native observation](evidence/t006-native-observation.md)、[numerical reanalysis](evidence/t006-numerical-reanalysis.md) | 组件证据不能代替生产数据来源和真实运行 | 仅重测变化的 oracle/collector 行为 |
| T006.b | T006 | collector 读取每次 User 留存的独立 graph-reference.json | IMPLEMENTED | [request wiring](evidence/t005-request-reference-wiring.md)；核验 run/request/runtime/placement/graph，缺文件拒绝共享图替代 | 仍依赖 T004.b 实际调度和完整 retained-native 验收；不以 helper 关闭父任务 | 与 T005.b 共用 176 项证据，不单独启动 GPU 采集 |
| T007.a | T007 | 生产路径设计—代码审计报告 | VERIFIED | [2026-09-08复核](evidence/design-code-convergence.md)：旧G1/G4已接，N3源码修复；当前N1/N2 HIGH，报告BLOCK | 完成host producer/validator与MiniNDN wrapper，保留旧发现为历史 | 复用已通过版本检查与旧focused证据；报告更新不触发全套验证 |
| T007.b | T007 | T002/T004/T005/T006 生产接线收敛为 PASS | BLOCKED | N1 implementation closed by [host-gate producer](evidence/t010-host-gate-producer.md); N2 still lacks a genuine post-Selection dependency run and same-campaign receipt | Extend the maintained MiniNDN owner with the registered dependency cutpoint, then produce one source-bound receipt; do not substitute Y-N-C placement failure | 只重审/重测变化边界；未PASS不开展正式T008+验收 |

### T008–T017：逐级取得运行证据

| Step | Parent | Concrete outcome / path | State | Evidence / verification scope | Blocker / next action | Reuse / rerun trigger |
| --- | --- | --- | --- | --- | --- | --- |
| T008.a | T008 | `tools/spec183_host_build.sh` 干净根构建驱动 | IMPLEMENTED | `64df1581`；[host-unit](evidence/host-unit.md) 已记录构建 exit 0 | 正式验收仍受 T007 和候选源码身份约束 | 复用匹配候选的构建产物，不无条件重建 |
| T008.b | T008 | NAC-ABE + NDN-SVS → NDNSD → NDNSF，系统 Boost 1.71，最多 -j4 | IMPLEMENTED | 其他客户端记录 BUILD PASS、隔离根与两个扩展；本轮未重跑 | 核对最终锁/source seal 与该构建身份；T007 未 PASS | native/toolchain 变化才重建对应 ABI consumers |
| T008.c | T008 | `_ndnsf` 与 `_py_repoclient`、真实入口、ldd/readelf/hash、注册 unit | BLOCKED | [host-unit](evidence/host-unit.md) 与4ade12bf已记录树外import/ldd、unit-tests和integration-tests RC0；本轮未重跑 | T007与最终source身份仍须闭合；二进制suite通过不等于T009多进程YOLO | 复用匹配候选的测试记录；不重复已有loader或unit集合 |
| T009.a | T009 | 多进程 CPU YOLO 正常 ACK/Selection→四角色→数值结果 | NOT_STARTED | V09；NOT_RUN | T008 后执行；bootstrap 与 inference 分开判定 | 不重跑全部历史 DI 集成 |
| T009.b | T009 | 当前 epoch/权限拒绝、错 Selection、activation loss/tamper 与清理 | NOT_STARTED | V10；NOT_RUN | 与 T009.a 共用 fixture，逐个保留独立判定 | 只跑注册安全/故障场景 |
| T010.a | T010 | MiniNDN 正常 CPU 图、权限拒绝、缺依赖三个注册场景 | PARTIAL_YB_YN_PASS | [v48 exact-SIF Y-B](evidence/minindn-v48-exact-sif-yb.md)；[v49 exact-SIF Y-N](evidence/minindn-v49-exact-sif-yn.md) 的 Y-N-O/C/P/R/I/E/L 全部 `status=PASS`，supervisor returncode 0、process cleanup clean；v48 terminal/numerical match 与四角色执行证据已重核 | Y-N-C 是 placement/no-feasible-candidate，不能关闭 post-Selection 缺依赖；同源 host receipt、GPU/Tiger资格仍未完成 | 复用同一SIF/app只跑未决场景；改变base/ABI/app行为才重新跑已通过矩阵 |
| T010.b | T010 | 同源 host qualification manifest 绑定命令、结果与清理 | IMPLEMENTED | [host-gate producer](evidence/t010-host-gate-producer.md)；v2 shared validator + producer focused 18 passed；真实 v48/v49 join 按缺依赖边界 fail-closed | 取得一个真实 `DEPENDENCY_DATA_MISSING`/`PEER_FAILURE` after Selection 输出，再从同批三场景生成 receipt | 生成清单不额外跑模型；不得把COMPONENT_ONLY改token冒充PASS |
| T011.a | T011 | development-20260907 source seal 与 definition 准备 | IMPLEMENTED | 后文 SOURCE_READY checkpoint：`2aea8a0e` / `c4f33beb`，非 SIF PASS | 后续源码改变须重 seal；旧锁不覆盖 | 纯任务表修改按输入清单判断，不无条件重建 SIF |
| T011.b | T011 | 构建或复用基础SIF，在匹配SDK构建独立app并验证组合闭包 | VERIFIED_COMPOSITION | [local-sif](evidence/local-sif.md) + [v25 exact-SIF Y-B](evidence/minindn-v25-exact-sif-yb.md)：基础6产物与外置应用闭包通过；同一SIF/app完成真实MiniNDN启动、协议和数值回执；app-only复用证据保留 | T011.c 的 empty HOME/scratch 专项、T010 完整场景、GPU/Tiger资格仍未完成；不把本地Y-B升级为正式qualification | 基础库/ABI未变复用SIF；仅应用改动重建受影响targets |
| T011.b2 | T011 | 同源应用补包复用与MiniNDN延迟导入闭包 | VERIFIED | [local-sif](evidence/local-sif.md)：r2包159文件，3二进制不变、compiled:false；6复用边界测试；本机系统Python导入成功 | 仅补包/导入范围；分层MiniNDN命令和真实场景仍待完成 | 不因缺辅助Python文件或本机工具环境而重编译/重建基础SIF |
| T011.b3 | T011 | Python应用改动复用C++构建缓存，冻结后实际命令加载 | VERIFIED | [local-sif](evidence/local-sif.md)：d9be0bfa应用，configure6.728s/Waf0.837s，三二进制哈希不变；冻结driver→SIF内User入口exit0；25focused通过 | 仅增量构建与实际入口；四Provider/MiniNDN推理仍待执行 | 基础SIF与未变C++均未重建；下一次使用最新app的缓存 |
| T011.c | T011 | exact-SIF 本地 CPU YOLO 与 empty HOME/scratch | NOT_STARTED | V13；NOT_RUN | T011.b 后执行，取得 LOCAL_CPU_PASS；v48 使用持久运行目录，未覆盖 empty HOME/scratch | 容器环境新增证据，不能以 host 结果替代 |
| T012.a | T012 | GPU/Apptainer/容量 substrate 实值清点 | IMPLEMENTED | [input inventory](evidence/input-inventory.md) 有早期 probe 记录，非最终环境资格 | 正式 allocation 仍需实测；早期 probe 不抵消 T007 | 静态输入复用，不重复下载/拷贝 |
| T012.b | T012 | exact-SIF staging、目标节点/GPU/路由与服务就绪 | NOT_STARTED | V14；NOT_RUN | T011 后核验实际 allocation 与哈希 | 每个新 allocation 检查环境，不重建同一镜像 |
| T012.c | T012 | 用户指定的独立 C++ NDN/SIF 两节点 CPU 诊断 | VERIFIED | [C++ NDN evidence](evidence/cpp-ndn-smoke.md)：209981，itiger01/02，同一历史 SIF 哈希，3条Data，0:0及清理；209980仅收尾标记超时，未记整次PASS | 仅基础传输诊断；T012正式GPU/权限/候选资格仍未完成 | 固定小例子不再跑；配置/ABI/网络相关变化才重测；复用实际日志与脚本 |
| T012.d | T012 | 同一真实YOLO模型的独立CPU/GPU参考 | VERIFIED | [backend reference](evidence/yolo-backend-reference.md)：CPU两次matched；209983，RTX6000Ada、CUDA kernel、两次数值matched、Slurm0:0及清理 | 仅STANDALONE_YOLO_REFERENCE；非NDNSF-DI或T013资格 | 209982失败保留；仅一次TF32修复对照，不重复相同参考 |
| T005.tf32 | T005 | 原生ORT与独立参考关闭TF32并绑定精度策略 | IMPLEMENTED | 真实GPU参考修复PASS；147组件+2拒错通过；实查1.20缺新C++ options owner，已改V2 C API并通过1项策略检查 | 新Provider仍需在SIF的1.20 SDK实际构建/装入app包及真实分布式请求；旧host1.26语法检查不证明该ABI | 不重跑未变CPU/独立GPU参考；验证变化native路径 |
| T013.a | T013 | 一节点 GPU，四 Provider，1 warmup + 1 measured | NOT_STARTED | V15；NOT_RUN | T012 后证明三模型角色实际 CUDA、Merge CPU、全图数值/清理 | 一次有界资格门，不扩展 GPU/模型矩阵 |
| T014.a | T014 | 两节点正常推理，1 warmup + 3 measured | NOT_STARTED | V16；NOT_RUN | T013 后证明 A backbone/merge、B heads 与跨节点依赖 | 同一候选第一次正常 allocation |
| T015.a | T015 | 一次远端 negative-dependency，Selection 后切断必需中间 Data | NOT_STARTED | V17；NOT_RUN | T014 后验证有限失败、无假成功及清理 | 保留唯一注册远端负例，不复制整套本地负例 |
| T016.a | T016 | 第二个新双节点 allocation，原配置/SIF，1+3 请求 | NOT_STARTED | V18；NOT_RUN | T015 后验证不改脚本的复用性 | 这是 SC-004 的独立验收，不是无目的重复 |
| T017.a | T017 | `docs/yolo-reusable.md` / README 最终操作指引 | IMPLEMENTED | 已有指引；尚无端到端合格交付证据 | T016 后以实际成功命令校对路径/配置 | 文档修正只做链接/契约检查 |
| T017.b | T017 | 离线重算与 `evidence/closure.md` 最终交付 | NOT_STARTED | V19；NOT_RUN | 对已保留结果重算，汇总各门和复用身份 | 离线重算不启动新的 GPU campaign |

### 当前关键路径与避免重复验证

方向审计：目标与 TigerCluster GPU YOLO 一致；原计划的 correctness/reuse 范围和
单节点 1+1、双节点两次各 1+3 已有界，不增加模型、GPU 型号或性能比较矩阵。
当前应先完成 **N1/N2 host资格producer/validator与MiniNDN wrapper → T007.b PASS**，然后
**T008 → T009 → T010 → T011 → T012 → T013 → T014 → T015 → T016 → T017**。
后文将 T008 称为“下一个实现块”的历史 checkpoint 不取消 T007 前置。
既有构建驱动初稿和开发探测不作为正式资格；本轮未干预可能存在的构建进程。

- 每次运行先声明“本次关闭哪一个未决条件、哪些输入改变、复用哪份证据”。
  无行为变化、失败或新风险，不重跑整个 focused 集合；子步骤表更新本身不触发测试。
- 沿用 [plan 的失效矩阵](plan.md#candidate-and-change-invalidation)（正文 `Changed plane`
  表）：native/toolchain 才触发 ABI 重建；配置/harness 改动重测受影响调用路径；
  SIF 字节变化必须重新做 exact-SIF 验证；新 allocation 只复用匹配的文件身份。
- 保留 unit、集成、MiniNDN、exact-SIF、GPU 和跨节点的不同证据范围；T010
  只跑注册三个场景，T015 只跑一个远端负例。失败先诊断修复，禁止反复运行等待变绿。
- 同一场景内合并采集数值、权限、角色/边、CUDA、退出和清理证据；不为每种
  evidence 文件独立启动一轮推理。既有失败与历史结果保留，不覆盖、不拼接 PASS。

## Historical Checkpoints

2026-09-07 T004 dispatch 级 profile/planes checkpoint：新增
[tools/spec183_dispatch_plane.py](../../../Experiments/TigerCluster/tools/spec183_dispatch_plane.py)
render/check —— runtime plane（sif b6710fd6 + nativeManifest 48d7ab79 +
libraryLock aad276e6）与 dispatch plane（effective-profile 文档 + 21 文件
sealed harness + 5 个真实 source）以 CAS hardlink 确定性重建，
[profiles/yolo-two-node.json](../../../Experiments/TigerCluster/profiles/yolo-two-node.json)
全部 12 个文件行回填真实 bytes/sha256。生产校验 check_chain 至 dispatch +
verify_harness VERIFIED（identities inputs 36c909db → runtime eeb8afa0 →
dispatch 10758dc3；qualification 仍未评估）。修两类工具缺陷：stage id 一律取
check_plane 的 canonical 文档 sha（误用 plane.json 文件 sha 会 render 绿而
check 红）；重建前需对 freeze 只读 harness 目录 chmod u+w。真实键集与
repo-top registry 一致性复验通过；删除无对应私钥的 TigerCluster/specs
孤儿 pub 副本。沙箱内 3.5GB SIF 冷读偶发污染（9008db7a 假象）见
failure-log，记录 identity b6710fd6 稳定正确。deferred：远端 storage
site roots 与 oracle 数值契约待 T005/T006/T012 wiring 验证；release.gates
仍空，local-cpu 运行资格待真实 prepare+local 执行 receipt。

2026-09-07 T010 驱动推进：新增 spec183_minindn.py 把 provision 产物
映射到 NDNSF_DI_YoloAckDriven_Minindn.py --case Y-B 的 11 个 env 输入。
四轮适配通过验证链：输出根排他（map 移入 inputs/）→ offer key map 的
容器路径重映射到 host 副本（digest 键权威）→ spec181 Y-B 布局部署
config（Spec183 身份 + MiniNDN 节点映射）→ NDNSF_SPEC180_CONFIG_ROOT
指向固定 key 集。剩余障碍：驱动深绑定 spec180 host 布局
（build-system-j2/spec180-native-build.json manifest +
di-native-provider 二进制），本机无 spec180 host 构建产物；Spec183
等价物（T008 干净根）尚缺独立 di-native-provider 构建与 manifest
适配——T010 保持 open，不做伪造 manifest。

2026-09-07 T008/T009 单测门达成（4ade12bf）：--with-tests 构建
500/500 任务成功（1h25m），build/unit-tests RC0 与 build/integration-tests
RC0（No errors detected，LD_LIBRARY_PATH 指干净根）。T009 剩余真实多进程
CPU 集成（test_yolo_integration.py 尚未创建）与 T010 MiniNDN 为下一
阶段：MiniNDN 环境齐备（minindn 包 + nfd/nfdc + Topology），驱动
Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-B 经 env 输入
（validate_inputs 布局待侦查，spec181 R004 保护纪元断言需按 Spec183
固定 key 集对应）。skill ndnsf-minindn-experiment 已加载。

2026-09-07 T008 host-unit 构建通过（64df1581/5171450d）：
spec183_host_build.sh 驱动干净根 -j2 全链——NAC-ABE（Experimental
5ed23e68，-O1 规避 GCC9.4 ICE）→ NDN-SVS（9f2d8a47，系统 Boost 1.71
按 AGENTS.md 规则）→ NDNSD（375a35c5）→ NDNSF Core（waf install）→
两个 Python 扩展（_ndnsf 显式闭包 env + _py_repoclient）。ldd 闭包全部
解析到 /tmp/t008-build-root（ndn-cxx 用系统 /usr/local），0 unresolved；
两扩展源树外 import OK（NDNSF_EXT_OK/PY_REPOCLIENT_EXT_OK）；
App_ServiceController entrypoint 真实执行到 NFD socket 边界。修复记录：
pythonWrapper setup.py 源路径改相对（pip 兼容，spec181 回归）、
NAC-ABE 仓库 example-trust-anchor.cert 排除、NDNSD 仓库 pkg-config
Cflags 修复提交（57d7431）。证据见 evidence/host-unit.md。T009 单测
（--with-tests）构建在跑；T010 MiniNDN 用 NDNSF_DI_YoloAckDriven_Minindn.py
+ Spec183 case 配置（skill 已加载），T011 构建等 host gate receipt。

2026-09-07 构建工具链规则（用户裁决，AGENTS.md 同步记录）：NDN-SVS 的
某些线在 configure 检查里声明最低 Boost 1.74，但本机是 Ubuntu 20.04
（系统 Boost 1.71，无法装 1.74）。手递锁 pin 的 Experimental revision
已声明 1.71 即可。规则：一律用**系统 Boost 1.71** + 锁 pin 的
Experimental revision 构建 NDN-SVS；不得为满足检查自建隔离 Boost 1.74
前缀，也不得在 checkout 里 patch 版本门。若某 revision 确实要求 1.74，
切换到锁里声明 1.71 的 Experimental revision（锁为权威），而非换新
Boost。此前为满足 master 旧版 1.74 门自建的 /tmp/t008-boost 已弃用。

2026-09-07 T011 构建前置推进：生成 development-20260907 锁（四仓库
revision 重 pin 当前 HEAD；20260906 锁作为 spec180 冻结输入不动）。修两处
构建前置：sealer 排除 NAC-ABE 未跟踪示例证书（examples/example-trust-anchor.cert，
不参与构建）；NDNSD 仓库提交其未提交的 pkg-config Cflags 修复（57d7431）。
prepare-development-handoff 现 SOURCE_READY（sourceSealDigest 2aea8a0e），
development definition 渲染完成（definitionSha256 c4f33beb）。build-local-sif.sh
的 Spec183 路径要求 --spec183-host-gate（lib/spec183_yolo_host_gate.py 验证
tiger-yolo-host-minindn-manifest-v1 PASS）——即 T010 MiniNDN 真实 host
receipt。T008 构建链（NAC-ABE+NDN-SVS → NDNSD → NDNSF → 两个 Python
扩展，-j2 干净根）是下一个实现块；host gate 不可伪造，T011 构建保持
NOT_RUN 直到 T008→T009→T010 闭合。

2026-09-07 T011 启动条件与输入演进：source seal 已成功
（prepare-local-sif-source.py：workspace.tar 455 文件 + nacAbe/ndnSvs/ndnSd
三个依赖归档，sealDigest 3603c895）。prepare-development-handoff.py
prepare 被 HANDOFF_CHECKOUT_REVISION 拒绝：committed
development-handoff.lock.json（= .cache 同名依赖锁）pin 的 ndnsf revision
是 Experimental@447f7584（development-20260906 冻结），当前工作分支
TigerClusterExperiments HEAD becf5550 含 Spec183 harness 修复
（apps/yolo.py 预绑定、identities.py 副作用清理等）。T011 构建必须包含
这些修复，否则新 SIF 仍带 controller 接口漂移。下一步：生成
development-20260907 新锁（release 字段 + 四个仓库 revision 重 pin 当前
HEAD，旧锁作为 spec180 冻结输入保留），inputs plane SOURCES 改指新锁并
重新 render 全链（identity 演进，fail-closed 重算），再重跑 T011
prepare → verify → render → build-local-sif.sh。wheels 已在
.cache/handoff/development-20260906/wheels/（5 个，与锁 sha 匹配）。

2026-09-07 T007 审计（report-only）：产出
[design-code-convergence.md](evidence/design-code-convergence.md)，判定
BLOCK (HIGH)。四层证据逐项核实（文档/CodeGraph file:line/913 测试/真实
容器 receipts）；登记 5 项 discrepancy：G1(HIGH) certifiedGraph 无已接线
production owner（prepare_role_reference 仅 COMPONENT_ONLY）；G2(HIGH)
base SIF 内 controller.py 无 --spec180-runtime-receipt-file（T011 重建
前置）；G3(HIGH) release.gates 空（hostMinindn/localSif 未产生，不得伪造）；
G4(MEDIUM) local/run/submit 有意 NOT_WIRED（设计内，wiring 后重审）；
G5(LOW) 远端 storage roots 未实测。解除路径：T005/T006 owner 接线 →
重审 PASS → T008→T009→T010→T011→T012→Tiger GPU 部署。

2026-09-07 run-local 开发驱动 checkpoint：spec183_dev_provision.py 新增
run-local 子命令，按 plan 驱动 run_rank 完整 local-cpu 生命周期。真实执行
到达应用启动层：SIF 内 NFD 24.07（ndn-cxx 0.9.0/Boost 1.71）真实启动，
4 条 nfdc 命令（status/strategy set multicast/face list/route list）全部
exitCode 0，tiger-yolo-network-setup-v1 receipt 落盘（candidateDigest
c7ec3d7f）。controller 启动被 SIF 内 DI 应用版本滞后阻断：镜像内旧版
controller.py 无 --spec180-runtime-receipt-file 参数而 host 源已有
（examples/.../controller.py:116）——记录为 T011 本地 SIF 重建的前置
发现（failure-log 同日条目），不做运行时文件注入。基础设施层真实证据
同时构成 T012 preflight 的一部分。

2026-09-07 首次容器内真实执行 checkpoint：新增
[spec183_dev_provision.py](../../../Experiments/TigerCluster/tools/spec183_dev_provision.py)
驱动 resolve_provision_inputs → stage_provision_inputs →
provision_run，在 base SIF 内以真实输入跑通完整离线 issuer：
Spec181 y-b.json 服务模板（templateDigest cda36d33）、spec183 重签的
canonical package catalogue（placementCandidateDigest 3fd5fb9d）、固定
key 集、真实 bundle（candidateDigest 8db00719）→ 真实
tiger-yolo-preparation-v1 receipt（receiptDigest 0ee275ff，30+ publicFiles：
8 张角色证书、case-policy、native-execution-plan、trust-schema、
runtime-publication、service-manifest）。执行中修三处 DI/运行时缺陷
（详见 failure-log 同日条目）：replay pythonWrapper 无编译扩展遮蔽
site-packages（_installed_yolo_owner 预绑定 ndnsf/py_repoclient）；
spec180 签名的 catalogue 与 Spec183 registry 不匹配（用固定 catalogue
权威重签至 .cache/model/spec183-signed/，sign_manifest 增 authorityId）；
ndnsf 扩展 import 在 $HOME 产生 .ndn keychain 副作用（issue() 仅清理
root 角色副作用，真实角色仍 fail-closed）。另修 container_command 的
--home 容器内绝对路径形式与 resolve_run_plan 的 --output cwd 锚定。
workload.descriptor 改指 specs/181 的 y-b.json，packageManifest 改指
重签 package（原指向 spec180 签名件属行角色错位，已纠正）。下一步：
repo-probe/worker 栈的真实 local-cpu 执行与 localSif gate 回填。

2026-09-07 T005/T006 独立参考生成组件：新增 prepare_role_reference，检查
组装模型字节摘要、ORT 版本/后端，以 BASIC/1-thread 创建独立 session 导出
优化图，不调用 run、不读被测 profile；只返回摘要/节点名，临时明文在私有目录
清理。真实小型 Add+Identity ONNX/CPU 测试核对独立执行 profile，测试环境为
onnx 1.17.0 / ORT 1.19.2，不代表目标 SIF/CUDA。相关 118 测试通过（1.38s）。
尚未接入实际 certified assembler/publication owner，返回值仅 COMPONENT_ONLY，
不能由调用者提供的 manifest 摘要推断其真实性；T005/T006/T007 保持 open。

2026-09-07 T006 判定修复：实际 Merge 是 native-yolo-postprocess，不能要求
ORT 图节点。曾复现 local/single/two 三种正常判定全部失败；现四角色执行/
依赖/数值要求不变，独立 ORT coverage 仅对应 BackboneNeck 和两个 DetectShard。
真实 join/comparator + 模拟 retained readers 回归覆盖 Merge 失败、缺 shard、
伪造 Merge-ORT；连同 collector/CLI 153 passed in 12.72s。独立图参考生产者
仍缺失，不关闭 T005/T006/T007，不代表 native/MiniNDN/SIF/GPU 通过。

2026-09-07 生产路径检查：certifiedGraph 目前只有消费者和合成 fixture，没有
已接线的独立 ORT/图参考生产者。已纠正注释中“生产者已存在”的错误表述；
T005/T006 必须补齐真实 owner、运行时 manifest 绑定和 collector 来源验证，
不得从被测 observations 反推 expected。具体步骤见
[certified-graph-owner-gap.md](evidence/certified-graph-owner-gap.md)。
SSH hostname 检查成功；本轮未提交 Slurm 或启动模型任务。

2026-09-07 identity 拆分调用链补漏：User run_requests 仍直接比较 placement
与 runtime digest；现改为通过绑定 receipt 分别验证，issuer 签发前匹配实际
catalogue 条目，宿主也重验 placement receipt 字段。71 项 application/
provision/prepare 聚焦测试通过（4.54s），含不同摘要正例、串错/缺字段负例。
密码学/model owner 使用替身；单节点公开执行流程仍未接通。

2026-09-07 profile→issuer 输入映射及小文件 staging：明确模板/package/registry/
私钥 locator/epoch/SIF/placement 摘要来源，生成 prepare-input-v2；重验公开
摘要、0600 私钥和独占目标目录，不复制模型或执行外部程序。8 项新真实文件
读写测试连同相关 profile/CLI/provision 回归 83 passed in 16.60s（合成 artifact，
不是真实密钥或 SIF）。最终 local/run 调用和运行资格仍未接通，T004 保持 open。

2026-09-07 准备接线审查：拆分放置候选摘要与运行候选摘要。内部 prepare-input-v2
分别传递 placementCandidateDigest/runtimeCandidateDigest，前者用于 offers，
后者用于 preparation receipt/worker。旧 v1 描述拒绝猜测转换；T004/T005 仍未关闭。
准备生产者/描述解析/进程调用/worker/operator 聚焦回归 32 passed in 2.73s；
生产者测试替换了密码学和模型 owner，进程测试使用假容器，不代表真实 SIF 资格。

2026-09-07 T004/T005 准备调用边界：新增 `yolo_operator.provision_run`，复用
现有 `apps.yolo prepare`、container_command 和 Processes；校验固定 descriptor、
public inputs、SIF/harness 摘要与隔离目录后，单次有界执行离线 issuer，保留
cleanup/logs，并从实际 public/preparation.json 重算摘要和输入绑定。10 项新
测试使用真实短进程、假 Apptainer/合成凭证；连同既有 prepare/operator 测试
24 passed in 2.28s。不是 SIF、密码学签发或模型资格。
尚未接入公开 local/run：最终调用者仍须从同一 profile/已验证候选解析
输入位置和 runtime 参数，并提供已注册的 artifact-policy-authority 私钥。
不得由此组件生成 READY 或绕过 T007/T008–T011。

2026-09-07 real prepare path: removed the impossible dependency on READY from
the content-only checker, which always reports NOT_EVALUATED. prepare now
freezes verified dispatch/harness bytes and an unqualified plan, with no
container, credential generation or remote command. The first unmocked CLI
test then exposed a missing run-directory creation; fixed with exclusive
creation before freezing. Runtime qualification gates are unchanged.
T004 remains open for actual credential preparation and allocation execution.
Focused command/bundle regression: 56 passed in 11.03s; synthetic artifact
fixtures only, no native, model, SIF or Tiger execution.

2026-09-07 actual model-input owner check: signed catalogue verification,
four-role catalogue registration, graph/initializer hashes and fixed
640x640/[1,50,6] reference validation passed on the existing local package.
The external legacy model-manifest summary is not the runtime-generated
canonical manifest; waiting for a replacement signature on that summary was
not justified by the YOLO runtime source. Full adapter import instead fails
on the missing host ndnsf._ndnsf extension. Dispatch manifest ownership and
native/runtime qualification remain open. See
[yolo-input-validation.md](evidence/yolo-input-validation.md).

2026-09-07 T006 retained-verdict regression: collect previously returned an
existing PASS without reopening evidence. Five mutation tests reproduced
false success for missing/invalid/changed handoff, forged qualification and
oracle rejection. collect now always re-runs its authoritative collector and
compares the complete result with the retained verdict without overwriting it.
35 operator tests passed in 8.40s. These are synthetic component tests;
T006/T007 and real model/network validation remain open.

2026-09-07 interrupted-dispatch review: corrected acyclic prerequisites
(hostMinindn → localSif → singleNodeGpu → twoNodeGpu), explicit HH:MM:SS,
case-specific node/task counts, typed GPU GRES and frozen-bundle wrapper path.
Rejected the unfinished sbatch enablement: generic PASS/READY receipt fields
do not validate staging or candidate identity. The command renderer is tested
but not dispatched; submit still stops before journal writes and Slurm.
Focused operator/profile/journal tests: 74 passed in 9.66s.
T004/T007 remain open for semantic receipts, shared-root staging, allocation
runner and unknown-job reconciliation; no native/SIF/Tiger run occurred.

任务按行为闭环组织，每项包含必要失败例→实现→聚焦回归→证据，不拆成“写测试/改代码/跑命令”三个机械任务。正式 broad suites/实验必须等 T007；focused red/green 可在实现中执行。所有 Tiger 文件以下用完整仓库相对路径。

## Phase 1: Setup

- [x] T001 Freeze receiving inputs and unresolved inventory in `Experiments/TigerCluster/docs/yolo-reusable.md` and `specs/183-tiger-yolo-reusable-experiments/evidence/input-inventory.md`: reconcile four repository revisions against `Experiments/TigerCluster/development-handoff.lock.json`, delivery-vs-runtime source, nine native outputs, actual public YOLO APIs, model/package/oracle hashes, local/compute Apptainer and proposed partition/GPU/resources/capacity. Register exact build and test selectors and expected errors from current code; keep missing items WAITING_EXTERNAL_INPUT and historical failures intact. No large build/download or model job. Read-only local inspection first; bounded substrate probe only under the reviewed exception in plan.md.

T001 evidence: [input-inventory.md](evidence/input-inventory.md)。完成清点而非物理输入资格；三依赖精确对象、本地base、签名package/oracle仍缺，compute探测按T007后例外执行。现有host gate绑定旧tiny-onnx、User正常路径一次请求的问题已分派下列任务。

## Phase 2: Foundational

- [ ] T002 Implement phase-specific candidate closure in `Experiments/TigerCluster/runtime/yolo_profile.py` and `Experiments/TigerCluster/tests/test_yolo_closure.py`: test wrong/changed hash, incomplete inventory, path escape, stale receipt, unbound script/env and stage bypass before implementation; enforce I/R/E identities and invalidation with no self-reference, inputs-before-build/runtime-before-execution/dispatch-before-upload gates. Prove rejected inputs make zero build/upload/staging/sbatch calls with spies at the real command boundary. Depends on T001 interface inventory; gates may report missing physical input without pretending it is present.

## Phase 3: User Story 1 - Reusable Launch Configuration

T002 partial checkpoint：[内容完整性实现/21项focused回归](evidence/t002-integrity.md)。check_plane/check_chain已实现，但旧builder dispatch、真实receipt和实际命令边界零副作用测试未完成；保持unchecked，不作为正式资格。

T002追加接收审查修复：在既有`build-local-sif.sh`与host-gate owner增加显式Spec183 workload dispatch；校验实际YOLO日志/数值/源seal的receipt，保留Spec175 v1/v2。新增回归要求未知schema、错workload/源/receipt或失败证据使Apptainer调用数为零。T010产生真实receipt，T011消费；不得填M01假清单，也不得绕过已有source/ABI门。

**Independent Test**: deterministic resolved config from another cwd; fail-before-side-effects; two concurrent submit attempts cannot both launch.

- [x] T003 [US1] Extend reusable role/container lifecycle in `Experiments/TigerCluster/runtime/baseline.py`, `identities.py`, and `yolo_worker.py`, with `Experiments/TigerCluster/tests/test_yolo_runtime.py`: preserve CPU baseline behavior; support explicit GPU/env/mount/cwd mapping, isolated role identities/PIB, owned process groups and bounded peer/readiness checks. Add real short-lived parent/child cleanup tests, wrong cwd/port/shared PIB rejection and no host runtime injection. Reuse common primitives, not a copied supervisor. Depends on T001 and the T002 content-integrity interface; final T002 qualification is required at T007, not before implementation of its consumers. Implementation/focused acceptance: [evidence/t003-worker.md](evidence/t003-worker.md); actual workload wiring remains T004/T005, production audit T007.
- [ ] T004 [US1] Deliver strict profile and one entrypoint in `Experiments/TigerCluster/profiles/yolo-two-node.json`, `schemas/tiger-yolo-v1.schema.json`, `jobs/yolo/submit.py`, `jobs/yolo/run.sbatch`, and `tests/test_yolo_submit.py`: expose the five contract commands, resolve every field into actual argv/env, reject leftovers, bind immutable bundle, stage-scoped gates and allocate-once journal. Test atomic duplicate-run prevention and SUBMISSION_UNKNOWN recovery without resubmit; register case differences and walltime/timeout budgets. Fill real operational values before enabling submit; do not modify old `profiles/two-node.json`. Depends on T003.

## Phase 4: User Story 2 - Current YOLO Path And Independent Verdict

**Independent Test**: existing application APIs and independent oracle exercised by focused tests; no copying an expected tensor into the distributed output.

- [ ] T005 [US2] Wire the existing secure YOLO application into `Experiments/TigerCluster/apps/yolo.py` and `runtime/yolo_worker.py`, with `tests/test_yolo_application.py`: reuse `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py` and installed native Provider interfaces; map the four roles without fabricating ACK/Selection, verify permissions/current status and direct predecessor Data, enforce native input and independent oracle contract. Focused tests must catch lost structured assignment, stale epoch/input, missing backbone-to-head Data, duplicate/late output, and forbidden shared-filesystem activation shortcuts. Any Core/DI repair belongs to its existing source/tests and updates candidate provenance. Depends on T004.

T005 partial checkpoint：新增 `runtime/yolo_operator.py` 作为唯一 rank-level production seam：校验已解析 run plan、准备收据摘要、bundle/public/output 边界、节点角色、端点、CPU/GPU 分配参数和 request budget；构造 `NodeRuntime.from_preparation`，为 startup/completion 使用同一 run/candidate/probe 绑定但不同目录的 `StartupBarrier`，随后只调用既有 `apps.yolo.run_normal_node`。它不生成 ACK、Selection、模型输出或 verdict。`test_yolo_operator.py` 已用 lifecycle double 验证拒绝路径和实际参数绑定；应用回归还验证正常 lifecycle 顺序和启动失败清理，完整 TigerCluster suite **798 passed in 33.44s**。证据见 [t005-operator.md](evidence/t005-operator.md)。本 checkpoint 不代表真实 SIF、Provider、MiniNDN 或 GPU 资格，T005 仍保持 unchecked，后续必须补真实 application/collector 接线和端到端证据。
- [ ] T006 [US2] Implement authoritative collection and failure oracles in `Experiments/TigerCluster/runtime/yolo_result.py` and `tests/test_yolo_result.py`: associate every request/attempt/plan/role/node/GPU/edge digest, validate numeric output via the existing contract, require all finite child exit0 plus controlled service shutdown/reap, and separate local/single-node/two-node/expected-rejection verdicts. Reject stale/forged PASS, missing role/response/CUDA, shape/class/tolerance mismatch, timeout-as-auth-rejection, nonzero worker, forced cleanup or corrupt output. Retain first failure and immutable reanalysis. Depends on T005.

## Phase 5: Design-Code Convergence

T005/T006追加调用次数约束：正常ACK-driven User一次只执行一请求。warmup/measured用独立User调用、request/attempt/输出目录及数值文件，Provider持续运行；focused test从生产入口证明实际调用次数，不能仅检查legacy sequential参数存在。

- [ ] T007 Audit production wiring before formal validation in `specs/183-tiger-yolo-reusable-experiments/evidence/design-code-convergence.md`: compare accepted spec/contract to actual submit→worker→application→Core/DI/Repo→collector and all effective fields through CodeGraph and exact source; register severity, owner and focused regression for each discrepancy. Review all wrappers/helpers and local/remote paths once as a closure, including scripts used by later substrate probes. Close only on PASS with zero controlling semantic/security/wiring/evidence gaps; actual changes reopen this task. Depends on T002–T006. This task is not satisfied by the planning audit.

### Accepted Layered Deployment Revision — 2026-09-08

本表是原任务的新增必要子步骤，不新增顶层任务，也不撤销T001/T003历史验收。
目标见FR-005/FR-006及[runtime layers](../../Experiments/TigerCluster/docs/runtime-app-layers.md)。
现有“九产物全在SIF”、text-only harness及旧R/E是待迁移实现，不是目标要求。

| Step | Parent | Concrete outcome / path | State | Evidence / verification scope | Blocker / next action | Reuse / rerun trigger |
| --- | --- | --- | --- | --- | --- | --- |
| T002.layer | T002 | 原source sealer/`runtime/yolo_profile.py`分离base/app闭包、显式layout版本和required R | NOT_STARTED | 仅设计；FR-005/006、Candidate Identity | 错base、app漏文件、旧layout混搭在副作用前拒绝；基础源码闭包不得遗漏 | app变更只失效E；基础变更才失效I/R |
| T004.layer | T004 | 原bundle/operator/worker/transport验证并只读挂载app原生产物，保留harness归属 | NOT_STARTED | 仅设计；`/app:ro`不覆盖基础前缀 | 连接builder输出到实际local/rank入口；app own DSO允许、基础库遮蔽拒绝 | 只传变化app；复用相同base、模型与既有运输owner |
| T011.layer | T011 | 原prepare/build definition/preflight拆base与app构建、清单及资格；SDK键隔离增量缓存 | IN_PROGRESS | 新base/SDK SIF d4031191…已构建并独立加载验证；普通/直接读取摘要一致；外部app清单4项拒错/内容检查通过，见evidence/local-sif.md | 编译外置app并验证完整组合；接正式profile/dispatch；旧输入读取故障仍仅有绕行方案 | 不重跑209981/209983；不把基础库PASS或旧monolithic receipt算完整组合PASS |
| T007.layer | T007 | 审计分层producer→manifest→transport→rank→collector全链及回退 | NOT_STARTED | 仅设计；既有T007 BLOCK仍保留 | 上三项接线后复审；错base/混搭/宿主库/旧回执均不可放行 | 复用未受影响组件证据，不启动文档性重测 |

UAV仅复用同一部署边界；本Spec不实现UAV应用或Spec182。正式YOLO运行继续等待
原安全、数值与runtime门，不用小例子或布局标签替代它们。

## Phase 6: User Story 2 - Formal Local Qualification

**Independent Test**: current dependency build, real CPU multi-process graph and same-SIF app path. Record one evidence receipt per independently meaningful gate.

- [ ] T008 [US2] Qualify the locked dependency/native/Python closure built locally in the matching base container/SDK, using existing build owners and `Experiments/TigerCluster/docs/yolo-reusable.md`, recording `specs/183-tiger-yolo-reusable-experiments/evidence/host-unit.md`: clean affected ABI consumers in isolated build roots, system toolchain/Boost, at most `-j4` per active build tree; record both Python extension imports, actual entrypoint checks, ldd/readelf/loaded hashes and relevant dependency/NDNSF/Tiger unit results. Reuse unchanged component evidence with its source identity; do not build a redundant host-ORT variant before building the actual SIF-ORT application. Preserve failures; no stale incremental objects or manual PASS manifests. Formal closure depends on T007 and complete T001 inputs; base construction follows T011's independent boundary.
- [ ] T009 [US2] Execute real multi-process CPU integration in `Experiments/TigerCluster/tests/test_yolo_integration.py` and existing NDNSF integration fixtures, recording `specs/183-tiger-yolo-reusable-experiments/evidence/integration.md`: separate identity/bootstrap tests from a prepared authorized fixture; real signed messages, encrypted dependency Data, actual small ONNX execution and final oracle. Cover fresh Controller/epoch, denied role, wrong selection, activation loss/tamper and process cleanup. No mocked inference final PASS. Depends on T008.
- [ ] T010 [US2] Run bounded CPU MiniNDN YOLO through `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` with Spec183-owned case configuration/driver in `Experiments/TigerCluster/tests` and record `specs/183-tiger-yolo-reusable-experiments/evidence/minindn.md`: normal four-role graph plus registered permission/dependency-failure cases, request-to-response IDs/edge hashes, route snapshots and cleanup. Freeze the actual source/base/app-bound qualification receipt for the composition's dispatch gate; it is not a prerequisite for constructing unchanged base libraries. Do not rerun all historical campaigns, build another host ABI variant or substitute echo. Depends on T009.
- [ ] T011 [US2] Split base-runtime and application production in the existing `Experiments/TigerCluster/adapters/slurm-apptainer/scripts/prepare-development-handoff.py`, `build-local-sif.sh`, definition and preflight owners; record `specs/183-tiger-yolo-reusable-experiments/evidence/local-sif.md`: build/reuse the locked base SIF, container/SDK-build only affected external app targets, freeze required-R app manifest, verify base/app DSO and Python closures and actual entrypoints in the exact read-only composition. Replace the all-nine-in-SIF assumption. Prove an app-only change preserves the base hash and rebuilds only affected targets; wrong base/ABI, shadowed foundational libraries and mixed legacy receipts reject before workload. Keep models outside both layers. Final composition CPU YOLO qualification depends on T010; base construction/reuse does not depend on each changed app's future receipt. No host-library injection or Tiger build. Use the existing bounded builder and at most -j4.

## Phase 7: User Story 3 - GPU And Cross-Node Execution

**Independent Test**: same immutable runtime with actual allocated GPUs; final graph must contain genuine cross-node model dependencies.

- [ ] T012 [US3] Qualify promotion and allocated environment through `Experiments/TigerCluster/jobs/yolo/submit.py` and `runtime/yolo_worker.py`, recording `specs/183-tiger-yolo-reusable-experiments/evidence/tiger-environment.md`: audit final script/profile closure before side effects; verify local/project/staged SIF equality, compute Apptainer, CUDA/ORT, writable capacity, roles/cwd/mounts, NFD and bidirectional signed Data/permission readiness. No build on Tiger/login-node inference, no runtime path patching, zero Provider launch on mismatch. Depends on T011 dispatch receipt; per-allocation subset repeats in every subsequent job.
- [ ] T013 [US3] Run one bounded single-node GPU job with all four Provider identities via `Experiments/TigerCluster/jobs/yolo/submit.py`, recording `specs/183-tiger-yolo-reusable-experiments/evidence/single-node-gpu.md`: 1 warmup + 1 measured real request, observed CUDA per model role, independent numerical oracle and clean shutdown. This closes only SINGLE_NODE_GPU_PASS; resolve concrete failures and rerun affected earlier gates before advancing. Depends on T012.
- [ ] T014 [US3] Execute first normal two-node allocation from the unchanged normal case in `Experiments/TigerCluster/profiles/yolo-two-node.json`, recording `specs/183-tiger-yolo-reusable-experiments/evidence/two-node-first.md`: actual distinct hosts, A backbone/merge and B heads, 1 warmup + 3 measured requests, both directions of dependency edges and full per-request identity/backend/numeric/terminal closure. Stop on first failure and preserve partial results. Depends on T013.

## Phase 8: User Story 4 - Failure Recovery And Reuse

**Independent Test**: precise failure preserved; subsequent clean allocation reuses the exact normal configuration with no hidden repair.

- [ ] T015 [US4] Execute one bounded registered `negative-dependency` job through `Experiments/TigerCluster/jobs/yolo/submit.py`, recording `specs/183-tiger-yolo-reusable-experiments/evidence/tiger-negative.md`: after genuine Selection and before one required activation delivery, stop the owning Provider or use the registered fault Provider to suppress that edge. Expect exact missing-dependency/peer-failure boundary, no successful User result, no reselection, finite abort and full cleanup. Local mutation/unit gates cover other corrupt config/auth/false-PASS combinations; do not expand a remote fault matrix. Depends on T014 and matching fault harness receipt.
- [ ] T016 [US4] Reproduce normal distributed inference in a second fresh allocation via the same `Experiments/TigerCluster/profiles/yolo-two-node.json`, recording `specs/183-tiger-yolo-reusable-experiments/evidence/two-node-reuse.md`: unchanged E/profile/normal-case/SIF/model/harness/oracle, new run/identities/allocation, 1 warmup + 3 measured complete requests, no prior-run private/cache correctness dependency and no per-run large copies. Repeat complete allocation preflight, reconcile both normal runs and first failure logs. Depends on T015; changing behavior requires new identity and requalification, not counting the changed job as unchanged reuse.
- [ ] T017 [US4] Deliver reproducible configuration, operator guide and closure in `Experiments/TigerCluster/docs/yolo-reusable.md`, `Experiments/TigerCluster/README.md`, and `specs/183-tiger-yolo-reusable-experiments/evidence/closure.md`: check all fields and real commands from a clean checkout, offline recompute results, document fixed versions/qualification scope/cache location/first-failure guide, per-run warmup and measured latencies, exact code/config/SIF IDs, pending limitations and safe cleanup. Register full SC/FR evidence and close only if normal repeated runs plus required negatives meet criteria. Depends on T016; no performance superiority claim.

## Dependencies And Execution Strategy

实现顺序：`T001 → T002 内容完整性接口 → T003 → T004 配置/冻结/提交状态接口 → T005 → T006 → T004 五命令最终接线/验收 → T002 集成验收 → T007 → T008 → T009 → T010 → T011 → T012 → T013 → T014 → T015 → T016 → T017`。
T005依赖T004已有输入/运行接口，不要求其尚待T005/T006才能实现的实际argv和collect提前完成。T004保持unchecked，最终生产bundle必须包含真实应用/collector/run.sbatch；禁止生成占位文件让冻结检查假通过。T007仍同时要求T002–T006全部闭合，不减少任何资格门。
T002 的真实启动边界来自 T004，receipt 语义校验来自 T006；原先要求 T002 全部完成才实现消费者会造成循环依赖。允许先实现消费者不等于放开资格门：T002保持unchecked，T007必须同时验收T002–T006。T010才产生真实host receipt；之前仅可用明确标注的测试fixture验证拒错逻辑。
T001 中的缺 artifact/GPU 访问不阻止使用已知接口进行 focused 实现，但所有实际 build/run 的输入必须齐全；不能跳过 T007 或伪造后续 qualification。

T001–T004形成 profile/launcher 实现骨架；可操作 MVP 还要求 T006 判定器、T002集成验收和T007审计，不能把骨架作为可提交实验的版本。完整用户目标到 T017 才结束。
无 `[P]` 项：当前关键路径共享 profile/runtime 和真实资格，默认串行；可在任务内部并行只读盘点/离线测试，但不并行构建或自动派发 agent，不同时运行多个同 candidate/gate 实验。
合并了“测试→实现→局部验收→证据”机械链；各任务边界来自配置、生命周期、业务接线、判定、生产审计和独立环境验收。

## Current Checkpoint

2026-09-07 Spec183 输入门关闭 checkpoint：T001 evidence（input-inventory.md）所列
缺口 —— `model-manifest.json` 仅绑定 atomic-v1 且无 signature envelope —— 已关闭。
现按同一固定权威重签 `shared-backbone-two-shard-v1`-bound model manifest
（candidateDigest `sha256:3fd5fb9d…dc891`，与四 role catalogue/offers 绑定；
`canonicalModelSha256`/`canonicalInitializerSha256`/`packageManifestSha256` 保留
Spec180 已核实的字段值，原始 atomic 文件未动）。签发用
`tools/spec183_authority.py sign`（model-manifest 权威
`spec183-model-manifest-ed25519-20260907`），产物作为接收输入放 ignored CAS
`Experiments/TigerCluster/.cache/model/spec180-public/`（payload 变体
`model-manifest-shared-backbone-two-shard-v1.json` sha256 `1255d95b…`，签名文档
`…signed.json` sha256 `02f7dabc…`；仅公钥/registry 提交）。签名经工具 verify 与
共享门 `scripts/spec180_contract_gate.py` 双通道验证通过，篡改 digest 被拒
（MANIFEST_SIGNATURE_INVALID）。该输入进入 dispatch/profile 的真实接线仍属
T004/T005/T006 任务，继续按 tasks.md 顺序推进。

2026-09-07 Spec183 experiment authority 修复 checkpoint（修整裁决已执行）：按用户
裁决，Spec183 不再等待丢失的 Spec180 离机私钥（原位于
`~/.config/ndnsf/spec180/*.key`，已不存在），改为自建固定 experiment-only 权威
key 集并永久复用——所有 run、所有 provider（catalogue / model-manifest /
offers×4 role）共用同一套。新工具 `Experiments/TigerCluster/tools/spec183_authority.py`
（`issue`/`sign` 子命令）持有私钥于 `Experiments/TigerCluster/.keys/`
（0600/0700、`.gitignore` 覆盖、绝不提交），公共侧
`specs/183-tiger-yolo-reusable-experiments/contracts/`：`catalogue-authority.pub`、
`model-manifest-authority.pub`、`offers/{role}.pub` 与
`trust-root-registry-v1.json`（schemaVersion 1、status CONFIGURED、keyId 绑定
`spec183-yolo-catalogue-ed25519-20260907` / `spec183-model-manifest-ed25519-20260907`）。
wire format 与共享门 `scripts/spec180_contract_gate.py` 完全一致（detached
envelope + canonical JSON），契约文档 `contracts/experiment-authority-v1.md`
固定此安排。13 项 authority 测试全通过（真实 pub/registry 一致性、真实私钥经
共享门互操作、双 authority 区分、篡改/未签名/外来 keyId 拒绝、幂等与权限、CLI
签名、gitignore）；门禁测试在重新 issue 后确认 catalogue 签名经 Spec180 gate
验证通过。凭此可直接签发 stage 2 candidate manifest 并进入真实 profile 接线；
T001 的 signed catalogue/model-manifest 输入不再受私钥缺口阻塞。该 key 集为
实验身份，不是生产 PKI；再生成会破坏全部已记录签名，须先记录 identity change。

2026-09-07 canonical Sync and component regression checkpoint：`applicationName + '/sync'`
（例如 `/appname` → `/appname/sync`）已由 profile、projection、NFD route 和 startup
validation 共用；不得从 Provider prefix 或旧 `/group` 推导。重新运行完整
`Experiments/TigerCluster/tests`：806 passed（32.84s）；仍为 component-only，未改变
T007 的 BLOCKED/NOT READY 状态。当前仍等待 candidate-bound signed model manifest、
Spec183 profile、native/SIF 和真实 MiniNDN/Tiger 证据。

2026-09-07 exact-SIF probe 修复：历史 base SIF 的 `ndnsf` 导入证明默认只读 home
会失败；preflight 现在为每次 probe 提供临时隔离 `--home`，相关 12 项 builder/
preflight 回归通过。该修复不改变历史 base 的资格，也不关闭 T008/T011。

随后重跑 TigerCluster 全部组件测试并合并 Spec183 preflight/builder 选择器：818
passed（38.15s）。这仍是 component-only 证据，T007 仍需真实 profile、candidate
manifest、native/SIF、MiniNDN 和跨节点运行收敛。

2026-09-07 输入接收推进：四个锁定源码已在隔离 `/tmp/ndnsf-spec183-src` 工作树按
`development-handoff.lock.json` 精确 checkout，五个锁定 wheel 已下载并逐一校验，
`prepare-development-handoff.py verify` 返回 `SOURCE_READY`，source seal 为
`sha256:9129d07298f5754823f3bc2bf9c10fea7416adbb1e4168ced3dc82750948612c`。
远端 Spec180 YOLO 包虽含匹配历史 graph/weights/oracle，但缺少 registry 引用的
`model-manifest-authority.pub`，且含私钥材料；仅保留为输入审计，不得进入 Spec183
公共 bundle。锁定 base SIF 已只读传输到本地 ignored cache，并按完整内容校验为
`sha256:b6710fd696a7f962f67f67a54278d92a15babb856c4af428a3f04ba54dc83285`；它仍
不能替代 Spec183 本地构建或 T007/T008/T011 资格。

T006 native identity checkpoint: the actual native reader now rejects missing,
malformed or wrong-role `modelDigest`/`artifactDigests`; nine negative mutations
first passed incorrectly and now reject. This is structural evidence, not a
certified-package comparison. Source audit found ORT_ENABLE_BASIC and
`session.disable_cpu_ep_fallback=1` unless explicitly allowed, but these settings
still need real runtime qualification. `bindNativeRunnerPreparationContext`
falls back from absent modelManifestDigest to planDigest: do not equate a valid
SHA-shaped modelDigest with independent model identity. Raw ONNX node counts
must not be compared directly with optimized ORT profile event counts.
T006 remains open for independently bound model/graph coverage and final operator
wiring; no build/SIF/Tiger inference gate is closed by these checks.
Focused checkpoint: **823 passed in 54.82s** across TigerCluster component tests
and the six registered Python backend/public-recipient/numerical/identity files;
JUnit: `Experiments/TigerCluster/results/t006-native-model-identity-r1/junit.xml`.
Synthetic observation mutations are not actual native model execution evidence.

Follow-up checkpoint: public retained assignments now use envelope
`yolo-public-assignments-v2`; each role carries canonical model-manifest and
artifact digests from the typed Selection assembly. The User writer rejects an
incomplete identity before creating the retained file, and the collector rejects
old/malformed envelopes or wrong-role model bindings. The complete focused suite
passed **843 tests in 45.51s**; JUnit:
`Experiments/TigerCluster/results/t006-graph-coverage-r1/junit.xml` (ignored
runtime output retained locally; the durable model-binding JUnit remains
`Experiments/TigerCluster/results/t006-model-binding-r1/junit.xml`).
This remains structural evidence, not an independent signed-package or
optimized-graph comparison.

The collector now accepts an optional externally staged
`tiger-yolo-certified-graph-v1` document. When supplied, it requires exact
per-role model/artifact bindings, backend, and the certified post-optimization
node-name set; it never derives expected coverage from Provider logs. Four-role
coverage tests passed in the 120-test retained/projection subset. Final operator
wiring must make this document mandatory before T006 can close.

Follow-up checkpoint: `finalize_expected_rejection` now provides a separate
fail-closed terminal boundary for the registered `negative-dependency` case.
It requires one committed Selection, zero reselection, a bound
`DEPENDENCY_DATA_MISSING` or `PEER_FAILURE` edge after Selection, no response,
and bounded non-forced cleanup. The focused retained-execution suite passed
**53 tests** and the complete registered focused suite passed **852 tests**;
JUnit: `Experiments/TigerCluster/results/t006-negative-rejection-r1/full-junit.xml`.
This is still component-only evidence: no real negative runtime was executed,
the operator collector is not wired to it, and T006/T007 remain unchecked.

The public component collector now owns the complete normal request loop via
`collect_normal_verdict`; it requires the registered warmup/measured reference
schedule and a certified graph before calling the fail-closed final boundary.
The resulting `tiger-yolo-final-verdict-v1` still represents component-only
evidence: the operator CLI is not wired to real retained paths, so T006/T007
remain open and no native/SIF/Tiger execution is authorized.

2026-09-07 retained allocation/GPU join: collect_retained_request now forwards
the external journal/profile allocation expectations to the dependency
collector. GPU node entries require trusted allocationDigest/gpuProbeDigest,
not caller-authored gpuBinding. read_retained_device_binding revalidates
run/prep/candidate, raw Slurm records, observed task data, probe nonce/log,
allocation link and probe-before-Provider launch order against the node receipt.
Two nodes must agree on job/step/host list/uid and have distinct hosts/UUIDs.
20 source-shaped real-file join tests pass. Trusted staging/final operator,
certified graph, negative path and real native/SIF/Tiger validation remain;
T006 is still partial, no runtime PASS. See t006-slurm-allocation.md follow-up.

2026-09-07 Slurm task binding: read-only site queries confirmed 24.05.2,
/usr/bin/scontrol and data_parser/v0.0.41. No active user job. Nonexistent job
and step queries return exit0 with empty/null records, so new validation
requires exactly one matching running record. capture_task_allocation and
NodeRuntime.verify_allocation check journal job/comment, uid, partition/GPU
type, hosts/ranks/tasks and selector before GPU probe or Provider launch.
Allocation JSON is candidate/run-bound; GPU probe links its digest. Actual
positive allocation test, trusted offline receipt join and final operator
inputs remain pending. See evidence/t006-slurm-allocation.md. No task closed.

2026-09-07 independent CUDA probe: run_normal_node now runs the finite
NodeRuntime.probe_gpu_device before network/Provider startup in GPU cases.
The frozen runtime/yolo_gpu_probe.py queries CUDA runtime count=1 and maps
ordinal0 through PCI to a driver UUID; no NVML-index shortcut. The same
container_command/selector/HOME/cleanup owner is reused, with a fresh nonce,
bounded startup budget and exclusive gpu-probe.json/log. A failed child cannot
qualify even if it prints matching JSON. This is CUDA visibility evidence,
not Slurm allocation attestation. Actual job/step/node receipt binding and
offline trusted consumption remain pending. No native/SIF/GPU test run.
Also fixed node receipts incorrectly requiring native Provider PID witnesses
for finite management/probe commands borrowing Provider HOMEs; actual
persistent Provider witness checks remain mandatory. T006 remains partial.

2026-09-07 retained device collection: the retained request -> dependency ->
role path now invokes validate_device_binding. GPU node entries require an
externally supplied gpuBinding {uuid, visible}; model roles receive their
owner node's binding, while Merge/CPU require no GPU binding and empty device
claims. A red real-reader fixture demonstrated CPU visibility was previously
accepted; corrected and covered alongside missing/mismatched CUDA bindings.
These are source-shaped records, not executed GPU evidence. The actual
allocation receipt producer and trusted staging are STILL PENDING, as are
live-Worker device join, certified graph and final operator. No task closed.

2026-09-07 device identity component: validate_device_binding compares native
CUDA UUID/list, CUDA_VISIBLE_DEVICES and runtime device ordinal against
independently resolved allocation/launch expectations; requires the native
cuda-runtime-pci+driver-uuid source. One exposed GPU means runtime ordinal 0,
even when its host selector is 3 or a UUID. CPU/Merge reject GPU claims or
visibility. Thirty-six source-shaped tests pass; no actual GPU probe. Next
wire independently verified allocation receipts and role mapping into the
final collector; do not derive expected UUID from the observation itself.

2026-09-07 PID binding wired: Worker now generates a fresh 64-hex nonce for
each Provider launch and executes the in-container witness before execing
the native Provider. Node receipt v3 retains launchNonce and host PID;
writer/reader require exactly one matching nonce/role/namespace-PID marker.
Native readers use that observed namespace PID while host PID remains the
cleanup identity. Live and retained collectors pass the nonce explicitly.
73 affected tests pass, including real namespace mismatch/reader regression
without skips. Earlier ordinary-process launcher fixtures now emulate the
bundle Python module lookup explicitly. Exact-SIF gate still required; this
is not Tiger qualification. Continue GPU/graph and final operator work.

2026-09-07 critical PID namespace audit: Apptainer --containall isolates PID
(confirmed by local installed exec --help); native getpid() is not the host
Popen PID currently used by role collectors. A real unshare user/PID namespace
test reproduced differing host/container PIDs. New yolo_launch_witness emits
a host-generated nonce/role/container PID and execs the Provider, preserving
that PID; included in required frozen harness inventory. Six actual-process
tests passed with no skip locally. NEXT REQUIRED: wire nonce/witness into
Worker launch, node receipt and native readers; do not merely drop PID checks
or disable isolation. Current collectors remain unsuitable for actual SIF
until this wiring is complete. T007 must block promotion on this issue.

2026-09-07 retained request join: collect_retained_request derives the User
output directory from node0 and the frozen invocation index, validates the
real lifecycle/numerical pair, recomputes role and Selection digests and count
bindings, then invokes four-role retained execution/dependency checks with
the lifecycle execution-plan identity. Runtime release candidate digest and
catalogue placement candidate digest are distinct explicit arguments. Normal
cases require attempt-1 (no retry/reselection). Nine tests use the real
lifecycle reader and explicit numerical/native doubles. This remains a
component join; GPU/graph/allocation/final normal operator pending.

2026-09-07 retained cleanup semantics: offline receipt reading now invokes
shared validate_cleanup_records (also used by live Worker cleanup), recomputes
cleanupSummary and verifies every frozen User invocation and unique request ID.
Seven red cases previously accepted hash-consistent but semantically invalid
receipts; now rejected. Sixty affected tests passed, including real OS cleanup
boundaries. This closes an offline composition gap, not final experiment
qualification; trusted staging, GPU/graph and complete operator remain pending.

2026-09-07 retained execution join: collect_retained_role_execution reads
verified node receipts, uses their PID/log binding, enforces request/attempt/
execution-plan identity and role-local current ORT profile. No reconstructed
Worker. collect_retained_dependencies combines the exact one/two-node role
layout, all four Providers and public DATA_V1 dependency agreement, comparing
log hashes again. Twenty-one tests added: real retained file readers with
fixture ownership/observations, plus explicit cross-node dispatch doubles.
Final normal operator, trusted staging, GPU/certified graph and full cleanup
receipt semantics still pending; no experiment PASS.

2026-09-07 node log receipt v2: actual write_worker_receipt now seals each
launched process's regular bounded log path/bytes/hash after cleanup.
read_node_log_receipt consumes transferred output independently of Worker
instances, requiring externally trusted receipt digest plus frozen plan,
candidate, preparation and rank bindings. It verifies exact launch fields,
log content and service coverage; no fabricated Worker ownership. Twenty
receipt tests passed with actual files and fixture ownership. Remote trusted
receipt collection, full cleanup/requests/allocation and final verdict remain
pending. v1 is historical and rejected by the new reader.

2026-09-07 owned dependency collection: `collect_owned_dependency_result`
requires all four roles from closed, reverified prepared Worker ownership
objects, correct node rank/mode coverage and actual launcher-derived paths.
It joins existing native PID/profile validation with public dependency checks
and compares per-log hashes across both reads. Nine join tests use boundary
doubles, alongside existing real OS process role tests; no native qualification.
ORT source audit confirms the configured post-Selection preparation factory
creates a new runner/session per invocation with unique profile prefix;
keep strict current-request profile binding. Warmup is not proof of reuse of
an already loaded ORT session. Complete operator/GPU/graph/final receipt join
and T007 remain pending.

2026-09-07 cross-role join: `read_public_dependency_contract` validates bounded
User evidence against external request/attempt/plan/role/Provider facts and
requires exact agreement of producer outputs and consumer inputs.
`collect_dependency_result` connects those expected edges to DATA_V1 log pairs,
with exact log-role coverage. APPLICATION_INPUT is retained separately: native
handler pre-satisfies it from authenticated request ingress, not dependency IO.
Tests use actual typed projections and synthetic log records; owned-PID/final
normal operator integration remains pending. T006 stays unchecked.

2026-09-07 User retention wiring: generic public projection now lives in
`ndnsf_distributed_inference/sdk/public_evidence.py` (no dependency from the
maintained YOLO example on Tiger harness files). `--retain-public-assignments`
defaults off; the frozen Tiger User argv enables it. Actual User helper checks
role coverage and all request/attempt/execution-plan/Provider bindings before
exclusive bounded 0600 output, never raw assignment wire. Nine retention
tests exercise the actual function with real typed Selection bytes and a
handle fixture. Cross-role collector join and native qualification remain
pending; no task completion claim.

2026-09-07 public dependency projection: `runtime/yolo_projection.py` now
decodes real typed V3 Selection wire and emits an explicit non-secret allowlist
bound to request/attempt/execution-plan/Provider. Native attempt-session and
multi-tensor/redistribution scope rules verified against source. Eleven new
tests plus eight log-pair tests passed; User retention and final collector
wiring remain pending. T006 stays unchecked; no native/SIF/Tiger PASS.
See `evidence/t006-dependency-pairs.md`.

2026-09-07 发现并修复V3 numerical planDigest错用outercarrier：新增handle.execution_plan_digest读取coordinator已绑定runtime摘要，YOLO numerical/终端marker改用该属性；不改任一digest计算。4属性kernel+数值related共41通过，native handle未验证。公开dependency projection仍待做；见evidence/t006-lifecycle-component.md。

2026-09-07 依赖trace启用NDNSF_DI_DEPENDENCY_OBJECT_TRACE=1；collector按外部sealed edges/session配对publish/fetch DATA_V1、role/name/scope/bytes，8新fixtures+Worker共39通过。须补真实sealed-plan public投影并绑定四条edge与planDigest，negative/实际跨节点仍未验证。见evidence/t006-dependency-pairs.md；T006未闭合。

2026-09-07 collect_request_result合并lifecycle与实际response重分析，严格对冻结graph/catalogue，numerical所用plan/result/request/attempt全部来自已验证lifecycle。4新增交叉绑定负例/正例，数值suite15通过；REQUEST_RESULT_COMPONENT_ONLY，native/GPU/edge/cleanup仍未闭合。

2026-09-07 two-rank real-file barrier回归：请求pending时对端不得close、正常双方退出、请求失败双方保留记录。修复跨phase failure盲区：completion持续检查startup失败但不复用旧deadline，finite User也监控common failure lane。6 orchestration通过，runtime/应用仍double，非NFD/SIF验证；见evidence/t005-normal-node-owner.md。

2026-09-07 internal normal node owner接线：network→startup→rank0 requests→both-rank completion→close→node receipt；completion独立目录/后启动预算、同run/candidate/probe绑定，User时限clamp剩余预算；失败通知+本地node-failure记录。3新orchestration double测试+application共45通过；实际双rank/negative/fullvalidator/operator/T007仍未完成。见evidence/t005-normal-node-owner.md。

2026-09-07 node回执组件：复核preparation、case/rank/output、全部persistent service与normal User index覆盖，cleanup通过后exclusive0600写run/plan/prep/candidate/launch argv hash与cleanup。9 contract测试通过；Worker/prep为double，完整probe/management/requestId/推理/edge/allocation仍须验证。NODE_CLEANUP_COMPONENT_ONLY，negative-dependency未放行；最终operator待接线。

2026-09-07 T006角色路径/启动关联：仅将/output子路径映射到role-owned输出，拒绝traversal/symlink/任意host路径；collect_role_execution从已关闭Worker唯一launch PID与日志接native+ORT校验，CPU/GPU/Merge三种分支。12 focused通过（真实OS PID/log/cleanup，合成execution/profile，非模型运行）。节点回执/完整inventory/物理GPU/edge/最终collector继续待办。

2026-09-07 cleanup校验对接真实NodeRuntime launch/close清单，不信任外部childCount；拒绝缺失/重复/PID错/forced/unreaped/lease残留/服务提前退出，有限请求必须exit0。12新用例且三种模式已有真实OS schedule测试接入，相关54通过（Apptainer仍double）。完整planned inventory/节点回执/最终collector待接线，T006未完成；见evidence/t006-worker-cleanup.md。

2026-09-07 T006接入bounded native日志唯一记录选择、ORT profile独立解析/节点分配逐项核对/请求绑定与文件hash，补CPU/Merge正例；相关46项通过。profile仅每session首次capture，warm复用行为需核验，旧profile继续拒绝；物理GPU/graph节点覆盖/edge/cleanup/最终collector仍未完成。详见evidence/t006-native-observation.md。

2026-09-07 native观察组件：发现旧collector不兼容Boost PropertyTree bool/uint64字符串且hardcode example identity；新增严格decoder+真实Provider/PID/request/attempt/plan绑定校验，29 focused通过。不把日志声明当实际GPU/edge证据；ORT profile/物理GPU/依赖/cleanup/完整collector仍待做。见evidence/t006-native-observation.md。

2026-09-07 GRAPH_READY catalogueDigest source接通：YOLO builder既有验签/图/权重校验后保存签名body摘要，V3发出它并拒绝非法非空digest。真实LifecycleJournal文件→collector回归通过，连同候选映射/边界共34 focused通过；仍须最终collector对冻结catalogue做精确比较及真实native验证。T006保持unchecked。

2026-09-07 V3 candidate身份source修复：Yolo26Splitter重建catalogue条目的runtime candidate，精确digest唯一匹配后返回注册ID/digest；V3 lifecycle调用resolver，runtime计划/命名/digest不变。4项隔离production-kernel回归通过；真实adapter/planner未验证。另发现GRAPH_READY缺少catalogueDigest，writer未拒绝缺字段，须由verified catalogue补齐；T006仍未闭合。

最终复核：timestamp巨大整数负例补齐后25项lifecycle测试，完整focused集合480 passed / 45.24s（results/t006-lifecycle-r2/junit.xml）；未执行真实native/inference，候选身份缺口仍待修复。

2026-09-07 T006新增bounded lifecycle组件：按维护中journal的10个事件严格核对外部case/request/attempt/candidate绑定、字段/顺序/计数/digest/有限时间，拒绝duplicate JSON、symlink与超限输入；24 focused用例通过。保持LIFECYCLE_COMPONENT_ONLY，不能证明Provider执行/edge/cleanup。此前发现的 V3 planner `PLACEMENT_DECISION` candidateId/candidateDigest 混淆已由 adapter 的 `describe_candidate_identity` 解析器修复，并由 `tests/python/test_spec183_candidate_identity.py` 覆盖；相关回归 **34 passed**。T006/T007继续unchecked；见evidence/t006-lifecycle-component.md。

2026-09-07 T006数值重分析组件已实现：User显式opt-in保留0600/≤1MiB响应bin并记录digest，Spec183启用且绑定prepared candidate env；离线重算真实响应而非信任matched flag。共用纯NumPy tensor decoder，adapter exports按需加载使oracle/codec不依赖_ndnsf导入；operator NumPy锁定1.24.4。扩展focused455 passed（35.86s），包括实际producer函数/codec/数学/文件和独立import进程，但参考值为fixture、无模型/native/SIF/Tiger运行。T006完整lifecycle/role/node/GPU/edge/cleanup及operator/T007待完成；见evidence/t006-numerical-reanalysis.md。

2026-09-07 configure_network已接入实际NFD/nfdc启动链，生产nfd-ready/routes-ready：配置端口/两侧endpoint绑定、真实socket类型检查、有限管理子进程、剩余startup预算、peer失败、无覆盖receipt；管理进程借用空闲Provider HOME，不与NFD并发用PIB。route_commands显式接收appName/sync；旧baseline默认/group保留。7新增组件测试（OS进程/socket真实，NFD/nfdc为double），总419 passed（26.61s）。最终operator仍须绑定真实allocation并依次调用configure_network→start_workload→真实requests/collector；T005/T006/T007未完成，无native/SIF/Tiger运行。详见evidence/t005-nfd-network-setup.md。

2026-09-07 启动协调组件接入Controller/Repo/Provider/双向探测实际helper，原子且run/candidate/probe绑定barrier、共享startup预算、peer失败传播；必须两侧network receipt匹配后才启动Controller，完整Provider ready后返回RUNTIME_READY。用户明确Sync为`/<appName>/sync`：plan.applicationName→runtime.application_name→group，不从provider_prefix推导；错`/group`路由拒绝。10新增组件用例，总412 passed（26.13s）。NFD实际路由安装/最终outer worker、完整collector/T007仍未闭合，无SIF/Tiger运行；见evidence/t005-startup-coordination.md。

2026-09-07 双向签名Data readiness组件已实现：apps/yolo_network.py，两rank并行、同一新probeId、真实RSA验签、精确name/payload、独立有限窗口；复用finite-role进程/HOME管理，探测不挂载model/GPU，退出后允许真正Provider启动。身份从prepared plan传入，不假设等于role名字。父层校验精确回执与退出，正式operator仍须同时接受两个方向；harness清单更新15文件。13项新增组件测试，总402 passed（25.00s），其中加密真实但Face是内存double。实际NFD/SIF/Tiger仍NOT_RUN；T005/T006/T007未闭合。详见evidence/t005-network-readiness.md；下一步完整worker协调/启动barrier及请求collector，不扩大实验。

2026-09-07 Repo readiness组件已接入真实NetworkDistributedRepoClient.capability()接口，normal/FirstResponding、禁用Targeted fallback；有限User独占HOME且不挂载模型。启动probe有独立nonce/回执/调用目录、monotonic预算及外层进程deadline，拒绝过期/异Repo/非零退出/symlink证据，cleanup完成才写READY。18项新增测试，总389 focused（22.60s）；native RPC仍为测试double，真实SIF/NDN运行NOT_RUN。跨节点readiness、完整operator和T006仍待完成；T005/T007保持unchecked，不提交Tiger作业。

2026-09-07 Controller publication readiness接入真实receipt共享校验器，MiniNDN和Tiger复用runtime/yolo_result.py，拒绝重复artifact行；宿主校验不导入SIF-only路径。Provider readiness绑定精确identity/单role READY，source已确认在权限安装之后，不代表model/CUDA ready。8项新增测试，总371 focused（22.30s）。yolo_result.py仅完成publication检查，T006完整collector仍待实现；正式operator、Repo/跨节点readiness及T005/T007仍未闭合。

2026-09-07 NodeRuntime.from_preparation已消费receipt/inventory，在构造前绑定mode/rank/role/run输出，并在每次role启动/User调用前重查；不在100ms存活轮询中hash。3项真实文件边界测试，总363 focused（20.70s）。正式operator仍须接用该factory并独立验证SIF/gates；直接constructor仅低层生命周期测试用途。T005/readiness/T006/T007未完成。

2026-09-07公开准备清单已写入receipt：精确文件集合/逐文件hash，拒绝未知目录、特殊文件、symlink、private PEM和缺失；verify_preparation绑定外部receipt hash及run/candidate再重算。10项文件fixture测试，总360 focused（20.85s），最终类型收紧后10项再次通过。外层operator/worker消费仍待接线，完整SIF准备及身份/模型资格未执行；T005/T007仍未完成。

2026-09-07内部prepare CLI/固定输入descriptor已接通，共享容器启动器只允许离线准备挂载/inputs，禁止普通worker/网络/GPU带入私有输入。处理Apptainer预建空root HOME，仍拒绝旧内容；7项边界测试通过。正式submit.py prepare资格入口、public manifest inventory/readiness/T006仍待闭合，完整native准备未执行。详见t005-public-recipients.md。

2026-09-07内部prepare编排已接入apps/yolo.py：固定空挂载、输入hash、原adapter签名图校验、真实issuer与各类材料、原Y-B policy/Repo权限/native plan和catalogue batch生成器。5项配置投影测试通过，总343 focused（20.50s）。完整prepare尚未实际执行，最终CLI/public manifest绑定/readiness/T006仍未闭合；不能根据代码接线宣称PREPARED或T005完成，T007须审调用边界、T008/T011实跑。

2026-09-07准备路径审查发现Controller会向只读/config写publication receipt；已增加显式receipt参数，Spec183改写/output/runtime-publication-receipt.json，输入保持只读且拒绝覆盖。338 focused通过（20.61s），文件行为测试使用真实函数体+模拟传输，不代表签名发布通过。Controller源码变化要求新source seal/SIF。准备主流程及readiness仍待完成，T005 partial。

2026-09-07 pinned trust导入组件完成：保留registry原始bytes和catalogue/model/authority公钥，校验candidate digest、public hash、epoch及authority私钥匹配，只将私钥放User HOME。`authority.pub`是native定位别名，不再要求重写registry的publicKeyPath；此前checkpoint该措辞已被修正。6项真实crypto/加载测试，总337 focused（21.89s），类型收紧后6项再次通过。尚需最终prepare接线、完整模型签名验证、runtime publication/readiness与T006；T005仍partial。

2026-09-07 offer准备组件已实现：四个独立Ed25519签名密钥，实际certificateName/keyLocator、Provider/service、candidate和Trust Schema绑定；复用独占写入和HOME lease。5项新增测试，总331 focused（20.04s）。尚未接最终prepare或真实ACK验证；下一步authority/catalogue输入认证和完整准备/readiness，后续T006。T005仍partial，详见t005-public-recipients.md。

2026-09-07身份issuer现从实际证书Data记录certificateName/keyLocatorPrefix，避免offer policy猜测名称；5项wire解析测试通过，总326 focused（21.32s）。这是证书结构解析，不是签名认证/实际issuer通过。下一步offer密钥及policy准备接线、模型/authority认证和readiness。T005仍partial，见t005-public-recipients.md。

2026-09-07真实recipient生成组件已加入既有identities.py：每Provider独立密钥/私有map，User独立requester seed和public map；独占写入及HOME lease，拒绝覆盖/身份重复。4项真实crypto测试通过，总321 focused（21.13s）。尚未接最终prepare；模型/authority/offer材料认证、readiness及T006仍待实现，T005不关闭。详见t005-public-recipients.md。

2026-09-07 native recipient启动接线已按真实C++环境变量完成，私钥map精确限定单Provider自身HOME；User和Provider共用`/config/contracts/trust-root-registry-v1.json`，publicKeyPath契约为`contracts/authority.pub`。新增6项拒错，317 focused通过（20.91s），证据追加于t005-public-recipients.md。下一步生成并认证这些真实材料及readiness；不能把fixture路径检查当作native grant通过。T005仍partial。

2026-09-07公共recipient接线：User支持public-only map，Spec183启动强制显式protected epoch及自身requester/authority密钥；311 focused通过。真实User seam新增两项测试因缺少`_ndnsf`在setup失败，未证明grant集成通过；T008须完整重跑。见[evidence/t005-public-recipients.md](evidence/t005-public-recipients.md)。下一步signed准备/registry布局/native Provider recipient/readiness，再T006；T005/T007保持未完成。

2026-09-07控制面接线：apps/yolo.py已复用Controller/Repo入口，role-local policy/store、显式capacity、bounded JSON拒错；见[evidence/t005-control-launch.md](evidence/t005-control-launch.md)。新审查发现现有User默认plaintext-v1且旧protected seam读取Provider私钥map，未满足Spec183隔离；下一步优先实现公共recipient key map并绑定显式protected epoch/requester/authority/native recipient输入。沿用既有可信in-process authority边界，不新增网络授权服务，不共享Provider私钥。完成后继续signed准备/readiness/T006；T005及T007保持未完成，不放行Slurm。

2026-09-07源码发现：YOLO声明CPU/CUDA备选，但_v3_role_specs只取首项，GPU-only模型Provider或CPU Merge无法同时满足。已在原DI coordinator保留明确的ONNX CPU/CUDA family后再由ACK选择；9个kernel测试复现/修复并保留资源/角色/CPU-only负例，见[evidence/t005-backend-selection.md](evidence/t005-backend-selection.md)。T008必须在原生绑定构建后以`SPEC183_REQUIRE_NATIVE_PLANNER_IMPORT=1`重跑该文件，禁止AST模式代替完整import。该源码变化使旧runtime/source seal失效；handoff交付SHA只作provenance，新SIF必须含修复。T005仍partial，Controller/Repo准备和T006未完成。

2026-09-07追加User接线：apps/yolo.py复用真实one-shot入口，NodeRuntime顺序持有User HOME并保留Provider，修正裸requestId与错误output预览路径。两节点1+3、单节点1+1均经进程边界聚焦测试；首失败停止。见[evidence/t005-user-schedule.md](evidence/t005-user-schedule.md)。T005仍partial；下一步准备signed material、Controller/Repo和真实permission/catalogue readiness，再T006。不得把测试validator替代正式collector；negative-dependency暂拒绝普通scheduler。

2026-09-07追加：T005 partial已将安装版native Provider启动参数接入NodeRuntime，四角色CPU/GPU与启动前拒错共15项新增测试；完整Tiger focused集合 **275 passed in 16.18s**。见[evidence/t005-native-launch.md](evidence/t005-native-launch.md)。下一步仍为真实应用coordinator、Controller/Repo/User准备与安全readiness、逐请求执行，然后T006；T005未完成，不放行T007/Slurm。

2026-09-07：T001/T003完成（2/17），T004 partial新增脚本bundle freeze/verify并接入dispatch真实入口，最新 **260 passed in 17.17s**，见[evidence/t004-harness.md](evidence/t004-harness.md)。下一步转T005实际应用/argv/cache/安全模型传输，再T006，回填T004完整五命令和T002资格。生产bundle缺真实应用/collector/run.sbatch，不制造占位文件；不新增Provider模型旁路挂载。三依赖精确commit已隔离接收，原工作树未改；source封装、签名模型包和本地base仍待完成。T007及全部正式环境门未通过，无构建/上传/Slurm/模型执行。

2026-09-07 T002 dispatch checkpoint：新增 Spec183 专用 host-gate receipt
validator，固定 `applicationName + '/sync'`（applicationName 自带前导 `/`）、四 Provider、shared-backbone
图和 normal/permission-rejection/negative-dependency 三类 case，并绑定
source seal 与 evidence 文件 hash。`build-local-sif.sh` 通过
`--workload-kind spec183-yolo --spec183-host-gate` 进入该路径；无效
schema/workload/source/receipt 或混用旧参数时，在 Apptainer version/build
之前拒绝。Spec175 原有入口与 version 命令顺序保持不变。host-gate 与
builder 回归 **22 passed**；仍为 component-only/side-effect-boundary
证据，真实 T010 receipt、T011 SIF 与 T007 生产审计未完成，T002 继续
unchecked。
此前 r3 合并 focused selectors 为 **874 passed in 51.75s**；JUnit 仍只记录
本地组件/命令边界，不能替代实际 MiniNDN 或 Tiger 运行。最新 r5 结果见
下方 preflight checkpoint。

2026-09-07 T002 preflight checkpoint：新增真实的两阶段
`ndnsf-di-spec183-preflight`。输入阶段验证 archive-backed source seal 和
完整 Spec183 harness（包括实际 `jobs/yolo/run.sbatch`）；SIF 阶段验证精确
digest/labels，并在候选 SIF 内检查 Python/native imports、ORT CPU provider、
entrypoints 和 `ldd`。缺失 harness、参数或运行时闭包时 fail closed；26 项
新增/相关边界测试通过。合并 focused selectors 后 **878 passed in 51.67s**
（`results/t002-yolo-dispatch-r5/full-junit.xml`）。这没有制造 SIF 或集群
证据；T002、T007、T010、T011 仍未闭合。
2026-09-07 application Sync naming hardening：新增
`runtime.yolo_profile.application_sync_prefix()`，由 projection、NFD route
setup 与 startup validation 共享校验 `applicationName + '/sync'`；拒绝相对名、
尾部斜杠、重复分隔符和 `/group` 退化。相关回归 71 项通过；合并注册 focused
selectors 后 **885 passed in 55.18s**。这是 routing-integrity/component evidence，
不是 native/SIF/MiniNDN/Tiger qualification；T005/T006/T007 仍未闭合。

2026-09-07 T007 production-wiring audit：新增
[design-code-convergence.md](evidence/design-code-convergence.md)。审计结果为
**BLOCKED / NOT READY**：组件边界和 885 项回归记录为 component-only，真实
profile、`run.sbatch`、五命令、候选 artifact、native/SIF/MiniNDN/Tiger 执行仍缺；
因此不提前关闭 T007，也不授权提交集群作业。

2026-09-07 T004 operator boundary checkpoint：`jobs/yolo/submit.py` 现在暴露
`check/prepare/local/submit/collect`，并由严格的隐藏 `run` 接收
`jobs/yolo/run.sbatch` 的 allocation。入口统一要求显式 profile、run-id、output
和 case；结构完整但缺少真实 dispatch/local-SIF/staging receipt 时只返回
`INCOMPLETE/NOT_EVALUATED`，不创建 run、不冻结 bundle、不调用 Apptainer/Slurm。
`prepare` 已接通未来 qualified dispatch 下的不可变 harness/run-plan 冻结路径，
`collect` 当时只绑定已有 verdict；当前真实 application/worker 仍未接线，
因此 T004 继续 unchecked。相关边界回归与 journal/bundle 测试 **57 passed**；
`run.sbatch` 已加入 harness 清单，但无 allocation 或未完成 T012 时 fail-closed。

2026-09-07 T006 collector handoff checkpoint：`collect` 不再只读取任意外部
`verdict.json`；它要求与 prepared run 绑定的 `collection-input.json`，严格解析
normal/expected-rejection 两种 handoff，并分别调用 `collect_normal_verdict` /
`finalize_expected_rejection`。成功结果带 `collectorSchema` 且原子不可覆盖写入；
收集失败只保留首个 `collection-failure.json`。新增 2 个命令边界回归，
`test_yolo_submit.py` 23 passed；这仍是 worker/fixture handoff 证据，没有真实
native、GPU、MiniNDN 或 Tiger receipt，因此 T006/T007 继续 unchecked。

2026-09-07 T006 handoff wiring checkpoint：新增
`runtime.yolo_operator.finalize_normal_collection()` 与
`runtime.yolo_collection.publish_normal_handoff()`。外层 coordinator 只有在完整
rank 返回后才能发布 `collection-input.json`；发布器重新读取并绑定每个真实
`node-receipt.json`，固定 node root、reference 目录、candidate/preparation digest，
GPU case 还要求 allocation/probe 文件和期望字段。6 个新增边界测试通过，随后
`test_yolo_submit.py` 23 passed；详见
[t006-collector-handoff.md](evidence/t006-collector-handoff.md)。这仍未连接实际
Slurm worker/collector，不能关闭 T006/T007，也不能声称有 native/GPU/MiniNDN/Tiger
证据。

2026-09-07 substrate input checkpoint：VPN/SSH 只读检查成功；远端 `bigTiger`
分区可见 `rtx_5000`/`rtx_6000`/`h100_80gb`，一个有界 `srun` 在 `itiger02` 实测
compute Apptainer `1.5.3-1.el9`，而登录节点仍为 `1.3.4`。因此本地 SIF 构建
必须等待并匹配 compute 版本，不能使用登录节点版本。远端项目目录未发现
Spec183 profile、签名模型包或 collector 输入，仅有旧 Spec170/Spec180 材料；
这些事实已记录到 [input-inventory.md](evidence/input-inventory.md)，不改变
T002/T007 的资格状态，也未启动模型或 SIF 构建。

2026-09-07 matching tool/input checkpoint：本机显式 `/opt/apptainer/1.5.3/bin/apptainer`
与 compute 节点版本一致；对锁定历史 base SIF 完成 `sif list`、label 和 `/bin/true`
执行。source handoff `verify` 返回 `SOURCE_READY`，临时 definition 可正常渲染，
但在 Spec183 dispatch/model gate 闭合前没有调用 `apptainer build`。从 Tiger 只读取
公开 YOLO package（不含 private key），逐项 hash 与历史记录一致，catalogue
Ed25519 signature 已验证；其外部 model manifest 仍错误绑定 `atomic-v1` 且没有签名
envelope，不能直接作为 shared-backbone Spec183 dispatch 输入。该残余缺口记录于
[input-inventory.md](evidence/input-inventory.md) 和
[spec180-candidate-reuse-audit.md](evidence/spec180-candidate-reuse-audit.md)；T004/T007/T008+
仍未完成，未提交任何 Tiger 作业。

2026-09-07 certified-graph document owner checkpoint：`runtime/yolo_graph_reference.py`
新增 `serialize_certified_graph(role_references, *, graph_digest)` 作为
`tiger-yolo-certified-graph-v1` 的唯一生产 serializer——消费 `prepare_role_reference`
原样返回的三角色记录（缺一即拒、跨角色单一 backend），在文档中嵌入
`referenceProvenance`（producer schema/qualification、确切 ORT 版本、optimizer
export digest、canonical session options、backend）。`validate_certified_graph_provenance`
逐角色重检 provenance，`validate_certified_graph_coverage` 在任何覆盖率比较前先调用
它：收集阶段伪造或剥离 producer 身份的 expected graph 直接拒绝
（CERTIFIED_GRAPH_PROVENANCE_*），不再被当作 trust 接受。角色边界由回归测试锁定：
`_ORT_ROLES` == `yolo_result._ORT_ROLES` == `yolo_worker.MODEL_ROLES`。测试：三个真实
小 ONNX 角色模型 → 真实 CPU 参考 → serializer 文档 → comparator join（expected 先由
ORT 准备独立固定，synthetic retained observations 只准随后匹配），另覆盖缺/多角色、
伪造 reference 身份/provenance、backend 混用/未知、node 词表与 graph digest 突变；
graph-reference 33 项通过，完整 TigerCluster **895 passed in 39.45s**。旧 comparator
fixture 已加明确标注 synthetic provenance。此 checkpoint 未改变证据分级：真实调用点
（从 signed candidate package 读 role model bytes、在 CPU 本机/已分配 GPU 上 prepare、
绑定 catalogue 发布后的真实 graph digest、经 preparation/run/collection 携带文档）仍
等待 T001 signed model manifest 与 SIF/GPU 门，T005/T006/T007 保持 open。

2026-09-07 inputs content plane checkpoint (d7c8dc6f)：`Experiments/TigerCluster/tools/spec183_inputs_plane.py`
render/check 把四个真实接收输入（sourceLock=handoff lock、sourceSeal、buildDefinition、baseSif
`b6710fd6…`）装配进一个被忽略的 plane 根（`.cache/planes/inputs`），逐行记录
path/bytes/sha256，并用生产验证器 `runtime.yolo_profile.check_plane` 检查为
`VERIFIED`（content integrity only，NOT_EVALUATED，不是 runtime qualification）。
文件用硬链接（禁止 symlink/`..`/absolute），重复 render 确定性一致；旧 plane.json
记录不同身份时 fail-closed 拒绝重写。5 项测试通过（含真实 repo 测试对 3.5GB SIF
的渲染+复核）。该门关闭的是 inputs stage 的收据完整性，不等同于 T011 SIF 构建或
dispatch 资格：真正的运行接线仍按 T004/T005/T006 继续。

2026-09-07 run-loop discovery checkpoint（探测，未提交任何作业）：对接收的
base SIF（b6710fd6…）做只读内部探测并核对 Spec183 提交栈实现状态。
（1）base SIF 内部确实可驱动：`/opt/venv/bin/python3` 3.10.18 导入
ndnsf / ndnsf_distributed_inference 0.111.0 / py_repoclient 成功，numpy
1.26.4 + onnxruntime（gpu-1.20.0）在位，DI 全家族 ndnsf_di_* 0.111.0 在位；
`/opt/ndnsf/bin/run-ndnsf-yolo.sh` 是节点内 Y-B 编排器（Spec180 Tiger
submission 形状，要求 SLURM_JOB_ID + args bundle 契约）——因此本地跑真实
执行**不需要先完成 T008 源码构建**，base SIF 就是开发循环的运行基座
（Spec180 T016 本地 replay 用过的 r119 镜像已不在本机，不能走该路径）。
（2）T004 五命令入口 `jobs/yolo/submit.py`（check/prepare/local/submit/
collect/run）已实现且刻意 fail-closed：prepare 冻结真实 harness bundle，
local 停在 `LOCAL_WORKER_NOT_WIRED`、submit 停在 `REMOTE_STAGING_NOT_WIRED`
——执行开关的正确开启方式不是删检查，而是先以开发回归方式跑出真实证据
receipt（localSif/hostMinindn 门），再把 receipt 写进 dispatch profile 的
release.gates。旧 `profiles/two-node.json`（tiger-two-node-v1）已带真实值：
sif 指向远端 `/project/tma1/…/spec180-runtime-b6710fd6`，sha 与本机 base
SIF 一致；T004 目标 `profiles/yolo-two-node.json`（dispatch 级）尚不存在，
是下一步要建的接线件。探测细节入 [input-inventory.md](evidence/input-inventory.md)。
本 checkpoint 不改变任何资格门；T004 仍 unchecked。

2026-09-07 artifact-policy authority checkpoint：Spec183 固定 key 集补第三个
签发权威 `artifact-policy-authority.key`（keyId
`spec183-artifact-policy-ed25519-20260907`，protectionEpochs
`["spec183-yolo-protected-v1"]`，grantSchema `ndnsf-di-key-grant-v1`），
作为**加性** owner 写入 registry（catalogue/modelManifest keyId 不变），
pub 提交为 `contracts/artifact-policy-authority.pub`。这是
`resolve_provision_inputs`/离线 issuer 对 dispatch 的硬前置：registry 必须
同时有 catalogue/modelManifest/artifactPolicyAuthority 三 owner，且 profile
的 `security.protectionEpoch` ∈ protectionEpochs。`issue` 语义收紧：新增
owner 不再需要 --force；改名已注册 keyId 仍必须 force。13 项权威测试 +
provision/trust 24 项回归 + 共享 contract gate 全绿。授权契约文档已同步。
