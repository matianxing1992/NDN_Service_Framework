# Tasks: NDNSF-DI Protected-Grant Local Development and Delivery

## Current Checkpoint (revision 7)

**Latest progress R19 (2026-09-06)**：`ce6a4ba0` 同源正式 Y-N 七子用例
全部 PASS，E 三真实 grant 变异全部在 Provider verifier 拒绝；九次
运行、63 个应用子进程退出均收集，零记录 PID/NFD 残留，source/input
前后不变。T005 完成，本机 **7/10**。见
[T005 current evidence](evidence/t005-y-n-matrix-current.md#current-r19-qualification)。

T008 三案例配置绑定已修复，真实 supervisor 与输入漂移定向检查
76 passed（6.22 s）；四组预存共享回归纳入，最终 32 passed（2.78 s，无 skip）。
Batch B 应用入口/Merge 两组最终 19 passed（2.88 s，无 skip）；剩余
来源已逐项处置；Batch C 核实 GPU 证据测试依赖未交付 CUDA 实现，
按 CPU 本地/后续 GPU 准入边界保留失败及草稿。Exporter/adapter/numerical
三组与共享 checkpoint 输入已纳入，隔离 R1 **41 passed（18.06 s）**，
包括真实 640 CPU ORT/PyTorch 数值对照；当前一项本地待纳入、
九项实验/历史草稿保留说明，不声明 GPU 能力已验收。
完整 C++ 构建 R1 因旧测试调用已移交的 `revoke/revoked` 停止；
已迁移到真实 grant fixture 的数据流/零化检查，最终 R2 定向构建
exit 0、30/30 用例和 225/225 断言 PASS。完整构建 R2 又在原生装配
集成测试的旧 `runtimeMetricsSnapshot()` 调用处 exit 1。该检查已
迁移至共享 preparation 与现有 execution evidence，R3 源注册链接
错误修正后，R4 完整 unit/integration 目标构建成功。三个具名装配
用例实际 ORT 加载/预热检查 3/3、138/138 断言 PASS（约 5.02 s）；
仅为定向修复证据，未运行完整 suite。三案例配置已落盘，生产配置
校验 3/3 PASS。输入身份缺口已修复：checkpoint 与 registry 引用公钥
均绑定实际文件，R1 语义 RED 后，隔离 R3 两文件 **92 passed（6.33 s）**。
其间 R2 暴露的共享 wrapper 输出目录依赖已限定纳入；显式 CLI 优先
及缺目录拒绝通过。三个实际注册 Ed25519 公钥摘要/身份检查 PASS，
纳入遗漏的 catalogue/modelManifest 公钥。详见 preflight 的 Input
Identity Closure R1/R2/R3。
下一步按 [preflight review](evidence/t008-local-suite-preflight-20260906.md)
完成剩余 candidate 本地交付工具/回归，封闭最终运行输入，复审后
以同一最终源码运行完整 gate。T008/T009/T012
仍未完成；T010/T011 保持 TRANSFERRED。以下旧检查点仅保留历史。

## Historical Wire Repair Checkpoints

**Wire native identity R1 (2026-09-06)**：`ce6a4ba0` 完整 native
build/verify 均 PASS；刷新后的 Core 上 21/21 与 270/270 断言
重跑通过，受影响 12 维度复审/A05 PASS。下一步 R19 同源正式
矩阵；T005/T008 未完成，本机 6/10。见
[native review](evidence/t005-exact-data-wire-repair-20260906.md#native-identity-r1-and-convergence-review)。

**Tensor wire R6 (2026-09-06)**：9/9 真实生产用例、270/270 断言
PASS；大 tensor 最大 signed Data 8,477 bytes，旧格式兼容和
七种拒绝边界得到验证。下一步提交共享修复、完整 native identity
刷新及复审；T005/T008 未完成，本机仍 6/10。见
[R5/R6](evidence/t005-exact-data-wire-repair-20260906.md#tensor-repair-r5-and-authenticated-inner-rejections-r6)。

**Tensor wire R3/R4 (2026-09-06)**：大 tensor 211/211 断言通过，
实际 202 包、最大 8,477 bytes。四种真实拒绝均通过；legacy
fixture 因旧元数据超包限制未到 decoder，先修正其分段大小再验证
兼容。详见 [R3/R4](evidence/t005-exact-data-wire-repair-20260906.md#tensor-repair-r3-and-negative-probe-r4)。

**Tensor wire R2 (2026-09-06)**：紧凑生产链成功重建大 tensor，
全部 signed packet 大小通过；412/413 断言通过，唯一失败为测试
重复转发造成包数翻倍。修正 fixture 转发后继续同判据及拒绝/兼容
回归，见 [Tensor R2](evidence/t005-exact-data-wire-repair-20260906.md#tensor-probe-r2)。

**Tensor wire R1 (2026-09-06)**：1,400,017-byte 真实生产 DI 回归的
旧 manifest 编码成 17,546-byte signed Data，被 Core 正确拒绝。
下一步按 [wire contract](contracts/exact-tensor-wire.md) 修复紧凑
codec/消费并验证兼容和拒绝路径，见
[Tensor R1](evidence/t005-exact-data-wire-repair-20260906.md#tensor-regression-r1)。

**Core wire R2 (2026-09-06)**：生产 Core 重建并以同一测试完成
21/21 断言 PASS，完整 signed Data 大小与拒绝批次的缓存可见性
已修复。下一步 DI 紧凑表示/旧格式兼容与先验边界检查；整体 A05
仍 BLOCK、T005 未完成，本机 6/10。见
[Core R2](evidence/t005-exact-data-wire-repair-20260906.md#core-repair-r2)。

**Core wire R1 (2026-09-06)**：真实 Provider/IMS 回归 18/21 断言通过；
8,801-byte Data 和超限批次错误成功，前项缓存泄露，3 项语义 RED。
下一步完整签名 wire 批量预校验并原样重跑，见
[Core R1](evidence/t005-exact-data-wire-repair-20260906.md#core-regression-r1)。

**Wire repair review (2026-09-06)**：候选紧凑消费存在旧格式字段覆盖及
transport digest 比较错误，资源边界也需前置。先闭合 Core 完整
signed Data 大小与拒绝批次的缓存可见性，再修复 DI 编解码/消费；
详见 [wire repair](evidence/t005-exact-data-wire-repair-20260906.md)。
A05 仍 BLOCK，T005 未完成，本机 6/10。

**Latest diagnosis R18 (2026-09-06)**：首次发送失败已定位为完整
NDN Data 超过 8,800 bytes：manifest 19,658 / 10,883，segment
14,191；169 条事件循环异常。下一步审查并验证既有共享紧凑
传输编码和批量发布前大小检查，保持签名/内容绑定；T005 未完成，
本机仍 6/10。证据见 [R18](evidence/t005-formal-matrix-20260906.md#exact-dependency-boundary-r18)。

**Latest attempt R18 (2026-09-06)**：BackboneNeck 完成实际 CPU
ONNX 执行；shard 获取其精确 tensor manifest 超时，Merge 随后
失败。矩阵仍未通过，身份不变、NFD 全清理。下一步修复角色间
精确数据传输边界；A05 BLOCK，本机仍 6/10。见
[R18](evidence/t005-formal-matrix-20260906.md#exact-dependency-boundary-r18)。

**Latest native refresh (2026-09-06)**：`214df1d6` 维护 native build /
verify 均 PASS，真实 backend 回归与受影响 A05 复审 PASS；允许
R18 新矩阵。T005 未完成，本机仍 6/10。见
[backend native identity](evidence/t005-formal-matrix-20260906.md#backend-native-identity-r1)。

**Latest backend repair R4 (2026-09-06)**：仅补齐公共 backend
名称注册，维护 Provider 重建后 5 项真实 CLI 检查 PASS（1.02 s），
包含 CPU 实际 load/warmup、未知名称与非法设备 metadata 拒绝。
下一步提交、刷新 native identity 并复审；T005 未完成，本机仍
6/10。见 [backend repair](evidence/t005-formal-matrix-20260906.md#backend-registration-repair-r4)。

**Latest attempt R17 (2026-09-06)**：四 Provider 已通过真实外部
assignment 校验并运行 handler；首个失败为 BackboneNeck 缺少
onnxruntime-cpu backend 注册。矩阵 exit 2，身份不变、NFD 清理
完成；A05 暂 BLOCK，先修复注册/消费边界。T005 未完成，本机
仍 6/10。见 [R17](evidence/t005-formal-matrix-20260906.md#native-backend-boundary-r17)。

**Latest native checkpoint (2026-09-06)**：`e6f44b65` 完成维护 native
重建及独立 verify，均 SPEC180_NATIVE_IDENTITY_OK；T007 受影响项
复审 PASS。下一步 R17 正式矩阵，T005 未完成，本机仍 6/10。见
[native rebuild](evidence/t005-formal-matrix-20260906.md#provider-digest-native-rebuild-r1)。
T008 前须核对主工作区尚未纳入隔离提交的 19 个 Spec180 测试及
其依赖/适用范围，不能将当前较小集合当作完整资格清单。

**Latest repair R3 (2026-09-06)**：Provider digest 规范化定向检查
4 passed（1.73 s），此前 4 failed（1.65 s）确认大小写不一致。
T005 仍未完成，本机 6/10；下一步提交该 hunk、重建 native 并复审，
再执行新正式矩阵。见 [digest repair](evidence/t005-formal-matrix-20260906.md#provider-digest-repair-r3)。

**Input**: [spec.md](spec.md), [plan.md](plan.md)，Spec 180 契约（继承）与
Spec 170 `artifact-assembly-v1` 契约。

**任务内聚规则**：一个任务 = 一个行为、一个 owner、一个验收门；测试
先行（focused failing case → 实现 → passing gate）。禁止机械拆分
"写测试/实现/跑测试"；也不得把两个独立行为（独立 owner/独立验收）
合并进一个任务。

**边界（2026-09-05 所有者决定）**：
- 撤销子系统（账本/网络服务/撤销校验）由另一分支开发；本 spec 不包含
  撤销任务，`revocationSequence` 固定为 1 的被动 wire 字段；
- 独立权威网络服务端（生产形态）为延期项；功能切片内权威运行在
  请求方进程中并复用既有 `ServiceUser.publish_signed_app_data` 发布
  路径（main 分支的明文路径证明"无网络服务"可工作，本切片不新增
  网络角色与前缀）。集成条件见 spec.md Out of Scope。

## Historical A05 Execution Checkpoints (revision 7)

**Latest diagnosis R16 (2026-09-06)**：Core INFO 日志定位四 Provider
在外部 assignment 准备阶段 size/digest mismatch，尚未执行模型。
当前沿发布/获取/校验链修复此边界；T005 未通过，失败保留。

**Latest attempt R15 (2026-09-06)**：首次到达四角色 Selection commit，
终端 REMOTE_RESPONSE_FAILED。当前诊断 Core/Provider 首次执行拒绝；
WARN 的 duplicate-request 日志尚不足以判定原因。T005 未通过。

**Latest repair R3 (2026-09-06)**：SealedCollaborationPlan 引用扩展
及严格值校验已完成隔离验证：22 + 36 项回归 PASS，A05 受影响项
复审 PASS。提交后恢复正式矩阵；本机仍 6/10，T005 未勾选。

**Latest attempt R14 (2026-09-06)**：共享绑定/装配闭合后请求到达计划
封存；SealedCollaborationPlan 的 fetch references 字段未进入提交。
当前修复公共计划契约的源码依赖，A05 暂 BLOCK；T005 未通过。

**Latest repair R6 (2026-09-06)**：共享 canonical binding、公共发布
与 CPU 装配依赖在隔离检出完成验证：40 项检查 PASS（12.91 s），
含实际 C++/Python parity；真实 YOLO 两候选的配方/发布 port probe
PASS。T007 受影响项复审 PASS；提交后执行新矩阵。本机仍 6/10。

**Latest attempt R13 (2026-09-06)**：epoch 修复后 User 进入 V3 request，
YOLO describe 依赖的 canonical binding 字段未纳入提交。A05 暂时
BLOCK，先验证并纳入共享 canonical 引用依赖；T005 未完成，历史
验收仍 6/10。

**Latest repair R2 (2026-09-06)**：子用例 epoch 已与 publication/
process specs 对齐，117 项 runner/matrix/grant seam 回归 PASS
（3.99 s），T007 受影响项复审 PASS。下一步新提交执行正式矩阵；
本机仍 6/10，T005 未通过。

**Latest attempt R12 (2026-09-06)**：提交 fixture 的隔离 loader 通过；
正式 Y-N-O 继承了只应供 Y-N-E 使用的 protected epoch，错误进入
grant seam 并缺 requester key。先定向修复子用例环境一致性；
T005 未通过，T007 的此受影响边界暂为 BLOCK，历史完成数仍 6/10。

**Latest repair (2026-09-06)**：既有固定 PPM/README 纳入源码，
实际 canonical package 的 reference loader PASS，22 项数值
回归 PASS（7.97 s），A05/T007 恢复 PASS。完成数仍 6/10；提交后
先验证隔离检出的实际输入，再恢复 T005 正式矩阵。

**Latest attempt R11 (2026-09-06)**：锁属主修复后 Controller 发布、
Repo 与四 Provider 就绪；User 因提交缺少固定 PPM 输入而退出。
A05/T007 对输入源码闭包重新 BLOCK；先纳入既有 fixture、定向
验证实际 reference loader 并复审，再恢复矩阵。T005 未通过。

**Latest attempt R10 (2026-09-06)**：NFD 与 Controller 启动通过，
Controller 内 publication ServiceUser 初始化报文件锁冲突；Y-N-O
CONTROL_NOT_PROVEN，矩阵 exit 2，源码/输入前后不变且已清理。
先修复此应用初始化边界；本机仍 6/10，T005 未勾选。见
[formal matrix](evidence/t005-formal-matrix-20260906.md#controller-publication-boundary-r10)。

**Status**: `IN_PROGRESS`。2026-09-06 按当前提交与证据复核；
T001/T002/T003/T004/T006/T007 的任务验收已闭合并勾选（本机 6/10；另有 2 个 TRANSFERRED）。其余任务仍按完整验收判断，
未勾选不抹去已实现的代码与 unit 结果。

| Task | Implemented / executed | Remaining acceptance |
|---|---|---|
| T001 | PASS：真实发布/消费与受保护装配、全部绑定、inline/external 清理及请求/排队生命周期；最终 151 项定向回归、6 个真实进程用例 PASS，见 [请求生命周期与完成审查](evidence/t001-request-lifecycle-20260905.md) | 本任务验收完成；正式 MiniNDN 归 T005/T008 |
| T002 | PASS：真实 native grant 正负链、资源/请求生命周期、公共准备与 adapter、handler 接线均闭合；追加生成接口修复后 48 cases / 366 assertions PASS，见 [完整验收映射](evidence/t002-acceptance-20260905.md) | 本任务验收完成；新源统一 manifest 刷新与正式同源资格归后续门 |
| T003 | PASS：3 项 grant parity 检查消费 9 个向量；8 个装配向量分别走 Python/C++ 生产入口，16 项检查通过，含实际 ORT CPU 结果和 initializer/recipe/ABI 拒绝，见 [装配证据](evidence/t003-assembly-parity-20260905.md) | 本任务定向验收已闭合；native 格式操作共用生产 Python helper，后续同源资格仍归 T005/T008 |
| T004 | PASS：7 项 Python seam、6 项 Core 定向检查；重建后真实 Provider 等待约 83 ms、Controller 12.39 s 就绪、等待中取消约 2.3 ms 且无热转；全部线程/网络清理，见 [生命周期证据](evidence/t004-lifecycle-acceptance-20260905.md) | 本任务定向验收已闭合；后续同源正式资格仍归 T005/T008 |
| T005 | 已停用旧自动重试入口，维护矩阵首个失败即停止并保留原始结果；118 项定向检查 PASS，见 [证据保留修复](evidence/t005-evidence-repair-20260905.md) | T007 PASS 后同源七子用例矩阵，保留所有失败 |
| T006 | PASS：153 项 Python、22 项 C++、3 rebuilt parity checks；r11/r12/r13 三种实际 Provider 拒绝及 r10 受保护正向控制通过；每次清理后 exit 0，见 [生产修复](evidence/t006-production-repair-20260905.md) | 本任务定向验收已闭合；同源正式矩阵仍归 T005/T008 |
| T007 | PASS：A01–A12 关闭，12 原则复审、完整 evidence inventory；最终提交源码/actual runtime 与 57 项应用回归对应，见 audit.md | 本任务验收完成；正式同源网络/清单归 T005/T008 |
| T008 | native 受保护 Y-B 定向正向控制已有证据 | T007 PASS 后执行同源本地资格清单与 Y-A/Y-B/Y-N 正式矩阵 |
| T009/T012 | 继承本地工具链，按修订 7 调整为开发交付/本地关闭 | 交付清单、复现/移交材料与 LOCAL_DEVELOPMENT_PASS 尚未完成 |
| T010/T011 | TRANSFERRED：SIF/replay/Tiger 归实验机器 | 外部验收仍未执行；不计本机完成率或本地关闭依赖 |

**Latest progress (2026-09-06)**：本机 **6/10** 已完成，另 2 项 TRANSFERRED。
T007 的 A05 Closure Matrix 与 12 原则裁决 PASS；`6b9bb51c` 最终源码/
actual native/application 核对及 57 项应用回归证明一致。当前进入
T005 七子用例同源矩阵，再执行 T008 完整本地清单。正式资格、
T009 开发交付与 T012 关闭仍未完成。
T005 正式运行 R1 已准备：维护 Y-N CLI、独立输出/state、显式环境
与前后源码/输入核对，见 [正式矩阵记录](evidence/t005-formal-matrix-20260906.md)。
运行结果待收集，任务保持未勾选。
R1 在 launcher git rev-parse 处失败：清空环境丢失 SUDO_UID，
Git 拒绝用户拥有的隔离检出。无网络启动；R2 保留真实 sudo 用户
身份后重试新目录，原始失败已记录，不改生产 source gate。
R2 暴露生产 `_source_git` 内部再次丢弃 SUDO_UID；在首次网络前
失败。A05 的 sudo 源码检查边界暂时重开，T007 回归待完成；
下一步真实 sudo 检出测试/定向修复后重新裁决，T005 未执行子用例。
sudo 源码边界修复最终 **51 passed（8.68 s）**，包含真实 root/
用户检出正负例、Git 覆盖隔离与既有 gate 回归。受影响 A05 复审
PASS，T007 恢复 PASS，完成数仍 6/10；下一步新 R3 正式矩阵。
R3 已通过修复后的 sudo 源码门，维护输入预检 exit 78：envelope
key owner 与 root 执行身份不符。下一步在新 R4 state 放置同字节、
0600 的 root-owned 副本；原 key 不变，未联网，不放宽输入门。
R4 envelope owner 通过，维护预检发现误用了 Y-B 四角色配置，
Y-N 要求另有 FullModel 能力。已记录 exit 78；下一步使用并核对
既有 Y-N 专属输入集，在新 R5 运行，保持注册角色要求。
R5 临时 launcher 未允许 Y-N 环境的 SPEC180_CASE_OUTPUT_DIR，
解析阶段停止，无网络。下一步接收该明确字段并由新 R6 输出覆盖；
已确认五角色配置/同一模型，并显式保留保护纪元以执行 Y-N-E。
R6 在 Mininet 可执行检查缺 ifconfig 处停止：显式 PATH 未含 sbin。
下一步核对完整系统命令并追加 sbin 路径，在新 R7 运行；保持
Python/native 原选择并重新记录环境摘要。未产生协议结果。
R7 五 NFD socket 存在但 nfdc 全部未就绪，Y-N-O 启动失败；清理
后无 NFD/native Provider。已定位 launcher 的离线 NDN_CLIENT_*
覆盖与节点 client.conf 冲突，下一步 R8 用独立父 HOME 隔离并移除
全局覆盖，保留显式依赖路径；本次不是协议负例 PASS。
R8 NFD/路由/keychain 已通过，controller.log 创建后立即启动失败；
矩阵包装丢失底层异常定位信息。下一步补类型/文件/函数/行号的
启动失败证据（不记录内容/locals），保持失败与清理，再定位修复。
诊断修复最终两文件 **93 passed**，部分启动清理与矩阵首失败停止
仍通过；新增类型/frame 证据不含异常内容/locals。受影响审计 PASS，
下一步提交并以新运行定位 spawn 根因；T005 仍未通过。
R9 新诊断确定根因是 Mininet.popen(shell=True) 读取缺失的 SHELL，
尚未真正启动 Controller。下一步 R10 显式设置 /bin/bash，保留
原矩阵/清理逻辑；R9 source/input 不变且无 NFD 残留。

| Closed unit | Current evidence |
|---|---|
| T002 acceptance / shared runtime | [完整验收映射](evidence/t002-acceptance-20260905.md)；[共用路径与差异 owner](evidence/shared-runtime-reuse-20260905.md)。共用准备与 adapter 已收口，生成 worker 授权修复为 48 cases / 366 assertions PASS |
| Qualification inventory | [范围与注册契约修复](evidence/t007-qualification-scope-20260905.md)：21 focused checks PASS；YOLO 三案例、活动 Spec 测试发现、案例路径/参数与输出 ID 边界已修复 |
| Evidence inventory | [完整逐文件清单](evidence/t007-evidence-inventory-20260905.md)，由维护脚本检查漂移；历史证据的层与用途保持各自范围 |
| Framework source closure | 提交 `1df718c8`；[26 cases / 204 assertions PASS](evidence/t007-framework-source-closure-20260906.md)，覆盖配套声明和 assignment 根名称传递 |
| Native projection closure | 提交 `1ba99000`；[29 cases / 133 assertions PASS](evidence/t007-native-plan-closure-20260906.md)，覆盖 COMPONENT_SET/后处理解析、根来源与多张量 scope/原授权组 |
| Committed native build | `1ba99000cd6b03705ea96f0b176d6a77fbbe8a1d` 的 tracked tree 无修改，维护 native build 与实际扩展导入/依赖身份 PASS；R3 receipt 见上一行证据。此结论仅限 host-local 构建，不是正式资格 |

## Historical A05 Repair Checkpoints

以下保留每次修复时的 BLOCK 与检查结果；当前裁决以顶端 Latest progress 和 audit.md 为准。

**Initial blocker**：[local gate identity](evidence/t007-local-gate-identity-20260906.md)
的源码单元 R7 已通过：39 项定向检查（3.44 s），错误 Git/源码
身份在子进程与输出目录创建前拒绝；执行期间源码变化使最终结果
UNQUALIFIED，保留全部子项与清理证据。实际 `1ba99000` 隔离构建
checkout 也通过只读源码校验。R1--R7 的失败和修复历史见上述链接。
后续核查生成构建工具、运行时/import 路径与外部输入字节绑定；
这些独立身份平面尚未闭合。T007 仍 BLOCK，
未运行正式矩阵；源码单元不新增已完成任务。
源码 checkpoint 曾被本地 hook 拒绝，已删除产品脚本中的助手目录
特例。R8 重验 39 项 PASS（4.46 s），真实 checkout 校验 PASS；
原 hook 保持启用，配置/工具链身份仍待关闭。
[启动配置单元](evidence/t007-local-config-identity-20260906.md) 已关闭：
共享 owner 从实际环境、工作目录、解释器字节、超时及输出策略计算
`effectiveConfigDigest`，在执行前后核对。R4 最终 **62 项定向检查
PASS（5.80 s）**，覆盖真实 child 环境消费、原 map 修改隔离、运行后
身份变化以及 builder/gate CLI 联通与配置漂移拒绝。原始 R1 六项
RED 与 R2/R3 修复证据保留。本单元不新增已完成任务，下一步继续
native/import 依赖、生成工具与外部输入身份；未启动正式矩阵。
[Waf 源/选择单元](evidence/t007-waf-tool-identity-20260906.md) R4
**80 项定向检查 PASS（1.75 s）**；主工作区和隔离 checkout 的实际
Waf 目录选择对照 PASS，各绑定 80 个源/资源文件。维护 native
owner 记录并前后检查 Waf 解释器/目录/源文件，child 显式 WAFDIR，
旧 receipt 缺字段时拒绝。R1--R3 的失败与修复保留在证据中。
下一步核查实际 import/外部模型输入，再用最终源码刷新 native
receipt；本轮没有 native build、模型或网络资格运行。
[显式配置目录修复](evidence/t007-explicit-config-root-20260906.md) R2
**24 项定向检查 PASS（0.85 s）**：受保护入口保留显式配置根，
在 child HOME/cwd 改写前解析绝对路径；显式目录缺 key 时直接
拒绝，即使默认目录有 key。R1 RED 保留。本轮未启动正式网络；
下一步继续外部输入字节/import 核查与 native receipt 刷新。
[输入身份单元](evidence/t007-local-input-identity-20260906.md) R4
**72 项定向检查 PASS（8.40 s）**：模型目录、配置、映射及引用
key 的实际字节由 inventory/gate 共同绑定；变化时保留已执行
结果并阻止后续网络案例/资格通过。C++/Python 选择器发现也使用
同一显式环境。R1--R3 历史保留；剩余为实际 import/runtime 核查
与 native receipt 刷新，本轮未运行正式矩阵，完成数仍为 5/10。
[维护 native refresh](evidence/t007-local-runtime-refresh-20260906.md)
在干净 `a51f87b3` 检出已 PASS（exit 0，新 receipt 已保存）；真实
应用 preflight 随后因缺少 `adapters.yolo.build_yolo26n_adapter`
导入而 BLOCK，尚未到 native guard/网络。下一步补齐提交内 Python
adapter 源闭包，保留 R1 再验；不能用工作区测试代替干净提交验证。
R2 选入四个既有 YOLO 包文件后，真实 catalogue verify 通过；
下一阻塞是隔离检出缺 `py_repoclient._py_repoclient` 构建。日志
已保留，下一步检查 Repo 构建入口并验证同源扩展；仍无网络启动。
R3 已启动 Repo 维护 setup 构建，显式选用与 framework receipt
相同的 SVS 头文件/库和隔离 framework 目录；最终 exit 0。
R3 Repo 构建/实际 import 已 PASS；随后公共 SplitCandidate 缺
`selection_priority` 字段，导致真实 publication 构造失败。已保留
日志，下一步关闭该共享契约依赖，不纳入无关工作区扩展。
R4 候选构造已通过，SDK 导入阻塞在公共 adapter 缺少
`MAX_INLINE_INPUT_BYTES`；已提交 coordinator 也依赖其
`InputTransportMode`。下一步补齐公共输入契约与导出，验证实际
应用路径；原始日志见 runtime refresh，T007 保持 BLOCK。
R5 公共输入契约依赖未提交的 `LargeDataReference`；下一步补齐
repo_reference.py 的 native publication 元数据绑定与引用校验。
这是公共 Python 源依赖缺失，尚未到 native guard/网络；证据已保存。
R6 SDK/Provider 导入通过，目录发布缺少公共
`PreSplitCatalogSnapshot.from_mapping`；下一步补齐该类型的校验与
双向转换。预检日志已保留，T007 BLOCK；未启动正式矩阵。
R7 runtime publication 已通过；process_specs 缺 legacy helper 的
显式 repo/py_dir 参数。下一步补齐本地 helper 配套参数化并验证；
此轮无网络启动，原始失败已记录。
R8 已通过实际 publication/process_specs/native guard；追加应用
入口导入检查发现 SDK 缺 `ProviderOfferTrustVerifier` 公开导出。
下一步补齐配套导出并复查全部入口；T007 BLOCK，未启动网络。
R9 确认 verifier 实现本身也未提交；下一步审查并纳入 SDK
provider.py 配套实现/公开生命周期入口，运行签名及公共 API 定向
检查。失败日志已保存；不得将 R8 native guard 视为完整源码通过。
R10 真实应用预检/四入口导入 PASS，无网络；六文件定向检查
9 failed / 36 passed（1.38 s），缺 DIRequestEnvelopeV2 输入传输
字段和 InferenceApplication 公共 task 参数。下一步补齐这两端
契约并重验，T007 BLOCK；记录见 runtime refresh。
R11 同一隔离源码的六文件定向检查 **45 passed（0.74 s）**，新增
候选 digest/priority/角色边界回归 **12 passed（0.44 s）**；真实
publication/process_specs/native guard/四个应用入口导入均 PASS。
57 项检查关闭选入的 Python 源依赖单元，保留全部 R1–R10 失败；
下一步在最终提交核对同源源码与 runtime，完成 T007 审查后才进入
T005/T008。完成数仍 5/10；本轮没有网络、SIF/Tiger 或 Git 合并。
源码单元 checkpoint 为 `6b9bb51c`，19 个 Python 文件与隔离已测
内容相同。R12 最终提交 source guard 拒绝 Repo setup 留下的未跟踪
中间对象目录；下一步移入原始证据目录保存后重验，不放宽源码门。
Context Mode project health PASS，active 层缺 session events，使用
仓库 authority；T007 完整审查仍待完成。
R13 保留 Repo 中间对象后，`6b9bb51c` 的实际 source guard PASS；
该提交的 production application/native preflight 与四入口导入再验
PASS，无网络。R11 的 57 项检查已与最终提交源码对应。下一步做
T007 的整体逐原则复审/A05 关闭裁决，通过后进入 T005；完成数未变。

**Execution order**：T007 PASS → T005 七子用例同源矩阵 → T008 完整
本地清单与 Y-A/Y-B/Y-N → T009 开发交付封存 → T012 本地关闭。
T010/T011 按 [交接契约](handoff-contract.md) 移交；不是本地完成条件。
Git 合并留到当前开发结束后另行讨论，不在本轮执行。
不得把定向测试或历史 PASS 代入这些未完成门。

**Shared runtime review (2026-09-06)**：已重新核对两种模型的 adapter
构造、生产准备 factory 的公共上下文绑定、生成路径向共同 worker
传递 guard；FR-015 已覆盖复用要求，保持现有模型差异 owner 与 YOLO
资格范围。同步修正共享说明中的旧 manifest/4 complete 表述，见
[当前源码核对](evidence/shared-runtime-reuse-20260905.md#current-review-2026-09-06)。
该复核发生于修订 6，完成数为 5/12；之后已按修订 7 分工调整活动分母。
当时验证：`audit_speckit_structure.py --strict` PASS（15 FR、6 SC、12 tasks、
5 complete、15 FR traced），`spec181_evidence_inventory.py --check` PASS，
`git diff --check` PASS；未重跑未改变的模型或 native 测试。

较早 4/12、6/7、7/7 与 CONDITIONAL PASS 均为历史状态，不覆盖当前
检查点。逐次 RED/GREEN、启动/构建失败、原始 run-id 与退役条件保留在
对应 evidence 及 Git 历史中；以 [audit.md](audit.md) 与
[修正证据](evidence/audit-repair-20260905.md) 为准。
本文件 `cpp/ndnsf-di/` 简写均相对 `NDNSF-DistributedInference/`；
`security/`、`core/` 简写相对其 `ndnsf_distributed_inference/`。

**Scope validation (revision 7)**：严格结构检查 PASS（15 FR、6 SC、
4 stories、10 active tasks、5 complete、15 FR traced）；10 个活动 ID
与 2 个 TRANSFERRED 的独立集合检查 PASS，修改文档链接均可解析。
唯一结构 warning 是活动 ID 不连续，来自保留 T010/T011 原移交 ID，
已明确说明，不重编号覆盖历史。范围调整没有关闭本地未完成任务。
`spec181_evidence_inventory.py --check` 与 `git diff --check` 均 PASS；
本轮范围文档变更未触发模型、SIF、Tiger 或 Git 合并操作。

## Validation Standard

每个实现任务的验收必须同时满足适用层，缺一不可：

T001--T004/T006 的完成门为其 unit/定向 integration 验收；这些任务
引用 T005/T008 的 MiniNDN 是后续 FR 资格覆盖，不是反向完成依赖。
只有标记 `[MiniNDN]` 的 T005/T008 由完整网络资格闭合。T007 审查
已完成的开发验收与后续资格设计，不要求未来的 MiniNDN 结果先存在。

1. **单元测试（unit）**：纯函数与编码层的 focused red/green + 变异
   用例（错误输入、边界值、篡改字段）。不得启动 NFD、不得跨进程。
2. **集成测试（integration）**：必须走真实生产调用链，禁止 mock
   替换被测链。每个用例必须声明三要素：
   - **生产入口**：被覆盖的真实入口（如"进程内权威签发经真实
     requester→authority 调用链"）；
   - **预期判据**：成功的判据或注册的拒绝原因（如
     `DI_PROTECTION_EPOCH_REJECTED`）；
   - **边界位置**：断言发生的授权边界（如"在 Provider 装配之前、
     verifier 之内"）。
   负例必须到达真实 verifier 并被其在授权边界拒绝；无关失败、错误
   生命周期相位、内部策略异常不得充当预期结果（Spec 180 假 PASS
   教训的制度化）。
3. **MiniNDN 小模型 CPU 测试**：适用任务（标注 [MiniNDN]）必须在
   MiniNDN 真实 NFD/NDN-SVS 上用小模型 CPU 后端执行，直至终端
   Response 或注册边界拒绝，全量子进程退出收集、零未收集存活进程、
   清理完整。

禁止：标签 PASS、seam-only 证据、以单元测试冒充集成测试、以集成
测试冒充 MiniNDN 资格证据。每个证据文件头部声明证据层（implemented/
wired/executed/measured）。

## Phase 0: Fail-closed Safeguards (Priority: P0)

这些历史修正防止合成拒绝、状态冒充与明文路径冒充保护执行。R 项
保留原 ID 与历史证据，不计入本机 10 个活动 T 任务的完成率。当前缺口由对应
T 任务完成定向修复；在生产验收前保持失败关闭，并禁止晋升诊断结果。

- **R001 Y-N-E Fail-closed Guard**（historical unit evidence；T006 负责生产验收）。在真实 grant 变异（T006 实现）之前，
  runner 的 Y-N-E 子用例必须报告 `UNAVAILABLE`（结构化原因
  `Y-N-E:GRANT_VERIFIER_NOT_IMPLEMENTED`），禁止合成纪元异常充当
  `PROTECTION_EPOCH_REJECTED`；删除合成拒绝路径。文件：
  `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`、
  `tests/python/test_spec181_y_n_e.py`。验收：unit（verifier 缺席时
  Y-N-E 报 UNAVAILABLE 而非 PASS/拒绝原因）；integration（矩阵驱动下
  unavailable 记录不进入 PASS 计数）。吸收关系：T006 落地后删除
  UNAVAILABLE 路径，由真实变异拒绝取代。

- **R002 Native Runtime Fail-closed Guard**（absorbed；T002 验收完成，以下保留先行门原要求）。在真实 grant 获取/
  解包（T002 实现）之前，native Provider 对非 `plaintext-v1` 纪元
  赋值必须失败关闭并给出明确错误（`DI_PROTECTED_GRANT_UNAVAILABLE`），
  禁止仅凭绑定比对进入 `GrantVerified` 状态；现有绑定比对函数明确
  为"绑定一致性校验"（改名或文档化）。文件：
  `cpp/ndnsf-di/ProtectedRuntime.{hpp,cpp}`、
  `cpp/ndnsf-di/NativeProviderHandler.cpp`、
  `tests/integration-tests/ndnsf-di-protected-grant.t.cpp`。验收：unit
  （C++ 负例：保护纪元赋值 → 明确 unavailable 错误，状态不进
  GrantVerified）；integration（native provider 真实保护纪元投影被
  拒）。吸收关系：T002 落地后由真实 grant 验证取代该失败关闭路径。

- **R003 Evidence Invalidation Inventory**（PASS，document inventory only；T007 已完成整体审计）。审计 Spec 180/181 全部
  证据文件：任何声称 PASS 但被后续修订失效的文件必须带失效横幅
  （t016/s1 已确认有；核查其余）；每个证据文件头部必须声明证据层
  （implemented/wired/executed/measured）。验收：完整清单 + 每文件
  层声明；当前修订不修改 Spec180 冻结文件，在本 Spec 的完整清单中
  记录其失效范围与替代证据。原 R003 记录的历史修改不在本轮重做。

- **R004 Protected-case Admission Guard**（absorbed；T001/T002 生产验收已完成，以下保留先行门原要求）。runner 的 Y-B 保护纪元子用例
  在 grant 接线（T001/T002）完成前必须失败关闭
  （`DI_PROTECTED_GRANT_UNAVAILABLE`），不得以明文路径冒充保护纪元
  执行、不得产出 PASS 记录。文件：
  `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`、
  `tests/python/test_spec181_runner_guard.py`。验收：unit（门禁存在）；
  integration（未接线时保护纪元用例被拒且原因明确）。吸收关系：
  T001/T002 落地后门禁转为正常执行。

## Phase 1: Protected Artifact Execution (Priority: P1)

- [x] T001 [US1] **Python Grant Publication and Consumption**。`provider.py`
  装配入口（`_assemble_certified_role_execution` 之前）在
  `protection_epoch != "plaintext-v1"` 时：按规范名**精确获取** grant
  Data（使用既有精确名 Data 获取原语，禁止 `ValidatorNull`）→
  `verify_and_unwrap_grant`（权威签名、绑定、过期）→
  `PlaintextLeaseRegistry` 注册内容密钥 → 装配 → 清理零化；任何校验
  失败以 `DI_PROTECTED_GRANT_REJECTED` 失败关闭（与 native 错误码
  家族统一）。请求方侧：`AuthorityBackedGrantProvider`（已实现，
  Spec 180 提交 `d36438c2`）进程内签发后，grant Data 经既有
  `ServiceUser.publish_signed_app_data` 路径发布，Provider 按同一
  规范名获取。权威私钥经注册表 `artifactPolicyAuthority` 条目加载
  （`~/.config/ndnsf/spec180/`，mode 0600）。**内容密钥真实消费
  （FR-013）**：装配产物按 `DISK_CIPHERTEXT_ASSEMBLED` 语义用派生密钥
  （`K_bundle = HKDF(content_key, ...)` → `K_entry = HKDF(K_bundle,
  entryKind)`，AES-256-GCM）加密暂存于工作目录；加载路径用解包出的
  内容密钥解密，明文分配注册进 `PlaintextLeaseRegistry` 并在清理/
  失败时零化。文件：
  `NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py`、
  `security/grant_provider.py`（复用）、
  `core/protected_artifacts.py`（复用；如需 AEAD 派生辅助在此新增）、
  `tests/python/test_spec181_provider_grant.py`。验收：unit（绑定/过期/
  跨绑定负例，复用 Spec 180 的 29 个编码测试为回归基线；AEAD 派生
  与加密/解密往返）；integration（真实 requester 进程内签发并经真实
  发布路径发布 →
  真实 Provider 进程精确名获取与解包：正确 grant 装配成功且密文
  暂存/解密加载/零化完整；错误收件人 grant 在 verifier 内被拒且无
  明文落地；**错误内容密钥或篡改密文在 AEAD 认证层以
  `DI_PROTECTED_GRANT_REJECTED` 拒绝**）；MiniNDN 由 T008 的
  Y-B 保护纪元子用例覆盖。证据
  `evidence/t001-python-provider-grant-current.md`。当前新增验收：授权先于
  `_assemble_certified_role_execution` 的明文/ORT 加载；签名有效但摘要
  不等于 Selection grant 引用时拒绝（定向回归已关闭）；Merge 模型
  预期值来自独立认证输入；所有 `.weights` 以 `EXTERNAL_DATA` 加密并
  登记租约，取消/失败时同样清理；核对 issuer 身份与注册表一致。
  必须新增真实进程测试 `tests/python/test_spec181_provider_grant_integration.py`，
  现有注入 fetch 的测试不满足 integration。

- [x] T002 [US1] **Native Provider Grant Runtime**。
  `NativeProviderHandler`/`ProtectedRuntime` 增加规范名精确获取、
  权威签名/绑定/过期校验与 KeyChain 解包（信封算法按收件人密钥
  类型：Ed25519 → X25519 转换、EC → ECDH-P256，与 Python 信封
  `alg` 字段一致）；绑定失败维持现有
  `DI_PROTECTED_RUNTIME_BINDING_MISMATCH` 语义并新增
  `DI_PROTECTED_GRANT_REJECTED`。pybind 暴露获取/解包所需的最小面。
  文件：`cpp/ndnsf-di/NativeProviderHandler.cpp`、
  `cpp/ndnsf-di/ProtectedRuntime.{hpp,cpp}`、
  `pythonWrapper/src/ndnsf/_ndnsf.cpp`、
  `tests/integration-tests/ndnsf-di-protected-grant.t.cpp`。验收：unit
  （C++ 校验负例：错误权威/收件人/绑定/过期）；integration
  （Python 端到端 native provider 真实解包，grant 由真实请求方进程
  发布）。证据 `evidence/t002-native-provider-grant-current.md`。
  FR-015 追加验收：复用现有公共准备/执行路径；native factory 的
  证据和资源初始化只保留一个 owner，模型分支只构造 runner spec；
  native YOLO 后处理计算归 adapter，并保持现有公开调用兼容。
  定向回归覆盖公共边界与受影响的已有生成接口，不能用复制 Qwen
  Provider 或新建平行运行时实现。见
  `evidence/shared-runtime-reuse-20260905.md`。

- [x] T003 [US1] **Grant and Assembly Parity Vectors**。固定向量文件
  `tests/fixtures/spec181/grant-vectors-v1.json`：同一 grant 字节
  （规范 JSON）分别由 Python 与 native 解包，必须得到同一内容密钥；
  向量含正例与全部负例（错误收件人、跨请求/attempt/core/model/纪元、
  过期、伪造签名）。文件：
  `tests/python/test_spec181_native_grant_parity.py`（消费 T002 的
  pybind 面）。验收：integration（双侧一致断言；向量文件随任何编码
  变更必须同步更新并重新双侧验证）。证据
  `evidence/t003-grant-parity-current.md`。FR-012 另需固定
  `tests/fixtures/spec181/assembly-vectors-v1.json` 与
  `tests/python/test_spec181_assembly_parity.py`：同一 canonical ONNX、
  recipe、external-data 与 backend ABI，分别消费 Python
  `assemble_certified_onnx_model`、native `NativeCanonicalOnnxAssembler`，
  逐字节及摘要一致；变异 recipe/initializer 必须拒绝。grant 的 9 个
  向量不能关闭该装配验收。
  定向验收已通过：Waf `spec181-assembly-parity` 调用真实 C++ 入口
  与正常 subprocess helper；8 个装配向量双侧验证 + 3 项 grant 检查
  共 19 PASS，见 [装配验收](evidence/t003-assembly-parity-20260905.md)。

- [x] T004 [US1] **Runtime Readiness and Cancellation**。`pythonWrapper/ndnsf/service.py`
  的 `start()`/`start_background()` 就绪等待改为 15000 ms（对 Core
  10 s 探针留余量）；`ServiceController.cpp` 的探针循环在
  `stop()`/取消后不得热转（取消检查 + io 停止后立即退出）。文件：
  `pythonWrapper/ndnsf/service.py`、
  `pythonWrapper/src/ndnsf/_ndnsf.cpp`、
  `ndn-service-framework/ServiceController.cpp`、
  `tests/python/test_spec181_controller_readiness.py`、
  `tests/fixtures/spec181/controller-lifecycle.py`、
  `tests/standalone/run-spec181-controller-lifecycle.py`。验收：unit（取消/
  停止路径）；integration（真实 controller 进程：超时余量下边界成功
  不被误报；取消路径及时退出且无热转）。证据
  `evidence/t004-readiness-boundary-current.md` 与
  `evidence/t004-lifecycle-acceptance-20260905.md`。

## Phase 2: Registered Negative Outcomes (Priority: P1)

- [x] T005 [US2] **Y-N Matrix Qualification [MiniNDN]**。在 MiniNDN 小模型
  CPU 上按注册语义重跑七子用例：Y-N-O（目录序无关）、Y-N-C（双候选
  不可行）、Y-N-P（ACK 签名/来源/绑定篡改）、Y-N-R（组件角色非法
  区间）、Y-N-I（非 ingress 获取，`DI_INPUT_FETCH_ROLE_MISMATCH`）、
  Y-N-E（真实 grant 变异，由 T006 构造）、Y-N-L（明文字段注入，
  redaction 违规检测）。每个子用例断言注册拒绝原因 + 边界位置；
  全量子进程退出与清理；无关失败不得充当预期结果。文件：
  `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py`（语义判定已在 Spec
  180 修复，本任务消费并重跑）、
  `tests/python/test_spec181_y_n_matrix.py`（矩阵参数化 + 原因断言）。
  验收：[MiniNDN] 七子用例全部预期结果、零未收集存活进程；证据
  `evidence/t005-y-n-matrix-current.md` 记录每子用例的拒绝原因与
  边界。Y-N-O 是 terminal control，不要求负拒绝码。
  `scripts/run_spec181_y_n_matrix_retry.py` 已停用：旧入口 exit 2，
  不删除目录、不启动进程、不生成结果。没有已验证的启动前故障
  分类器，不保留自动重试。唯一矩阵入口为维护 runner，首个失败
  立即停止、保留原始证据；诊断与 failure index 更新后才可使用新
  run-id 重跑。正式矩阵需保留全部失败与源/构建/配置摘要，在
  T007 PASS 后执行；旧自有 retry schema 不具备资格效力。

- [x] T006 [US2] **Production Grant Mutation Rejection**。构造三种真实 grant
  变异（过期、错误收件人、伪造权威签名）供 T005 的 Y-N-E 子用例
  使用，断言已实现 verifier 在授权边界以
  `DI_PROTECTED_GRANT_REJECTED` 拒绝；删除合成纪元异常路径。
  撤销仍是另一分支的延期项，不以过期或跨纪元拒绝冒充撤销。文件：
  `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` 的 Y-N-E 子用例、
  `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py`、
  `security/grant_mutations.py`、`cpp/ndnsf-di/ProtectedRuntime.cpp`、
  `tests/python/test_spec181_y_n_e.py`、
  `tests/python/test_spec181_production_grant_mutations.py`。
  验收：unit（三种变异构造）；
  integration（每种变异经真实发布/获取到达选定 Provider verifier，
  在装配前拒绝；绑定 request/attempt/provider，正向控制必须成功；
  无关 ValueError、未配置、超时或错误生命周期不得算预期拒绝）。证据
  `evidence/t006-y-n-e-grant-mutation-current.md` 与
  `evidence/t006-production-repair-20260905.md`。

## Phase 3: Convergence and Local Qualification (Priority: P2)

- [x] T007 [US3] **Design-code Convergence Audit**。按 12 审计原则对真实生产链
  （进程内权威、grant 解包双侧、装配、runner、本地验证/交付工具链）做
  code-aware 审计，四层证据分离；BLOCK 项修复 + focused 回归 +
  重新审计至 PASS。文件：本目录 `audit.md`、`traceability.md`、
  `evidence/post-implementation-audit.md`。验收：审计 PASS 且每个
  发现附 file:line 证据与关闭回归；四层声明在每个证据文件头部。
  T007 不等待 T005/T008 的正式资格结果；其审计 PASS 是这些执行的
  前置条件。R003 的完整证据清单与临时诊断路径删除条件由本任务核查。
  FR-015：附 YOLO/Qwen 共用路径与差异 owner 映射，核查普通角色、
  缓存和可选生成 epoch 的公共授权/截止/清理边界；共享修复附
  受影响的 stateless/stateful 接口定向回归。Qwen 模型资格仍在范围外。
  修订 7：继续验证实际本地源码/配置/依赖、构建与运行身份；最终
  SIF 字节、Tiger 配置或实验结果不属于本机 T007 的关闭前置条件。

- [ ] T008 [US3] **Local Qualification [MiniNDN]**。从 T007 PASS 的同一源
  身份（提交哈希）执行：单元/集成选择器清单（local-suite inventory，
  逐项子进程监督）+ MiniNDN Y-A（原子候选、1 Provider）、Y-B
  （共享骨架、4 Provider、含 T001/T002 的保护纪元 grant 往返）、
  Y-N 全矩阵（T005 同源重跑）；CPU 后端证据记录，不得呈现为 GPU
  证据。文件：`scripts/spec180_inventory.py`、`scripts/run_spec180_local_gate.py`（路径沿用，所有权
  移交本任务）、`evidence/local-qualification.md`。验收：清单完整、
  Y-A/Y-B/Y-N 全通过、零未收集存活进程、CPU 后端声明。

## Phase 4: Development Delivery and Local Closure (Priority: P3)

- [ ] T009 [US4] **Development Delivery Seal**。以 T008 已验证提交封存
  开发交付：源、契约、注册表（含 `artifactPolicyAuthority` 公钥
  摘要）、模型、oracle、runner、测试/parity、本地有效配置、构建/
  依赖及全部验证证据摘要。附可复现命令、外部工件获取说明、已知
  限制、未纳入交付的工作区修改说明，以及实验 owner 和反馈字段。
  文件：`scripts/spec180_candidate.py`（沿用并限定本地交付平面）、
  `handoff-contract.md`、`evidence/development-delivery.json`、
  `evidence/t009-candidate-seal-current.md`。验收：unit（脏树、摘要漂移、
  跨版本证据拒绝）+ 本地交付完整性检查；所有路径/摘要可解析，
  无秘密入库。SIF 输入封印与远端接收回执均不作为本任务前置条件。

- [ ] T012 [US4] **Local Development Closure Record**。同一交付身份下
  映射全部活动 FR 到实现、三层验证或交接材料；T001--T009 全部
  适用验收通过后，在 `evidence/closure-record.md` 发出唯一
  `LOCAL_DEVELOPMENT_PASS`，交付状态为 `READY_FOR_EXPERIMENT_MACHINE`。
  明确 T010/T011 TRANSFERRED，禁止声称 SIF/Tiger/GPU、Qwen 或
  性能资格；不声称已发送版本或远端已接收。验收：FR/SC/任务/证据
  映射完整、本地身份一致、移交责任可追踪，历史 PASS 不复用。

## Transferred Experiment Work (External Owner)

以下保留原 ID 与验收，owner 为实验机器，不纳入本机活动任务计数。
TRANSFERRED 表示责任移交，不是实现/执行/验收完成；字段见交接契约。

- **T010 — S4 Exact-SIF Y-B Replay — TRANSFERRED**。实验机器以本机
  交付的明确 commit 构建 SIF（含 rev-123 就绪修复后的运行时），
  执行 exact-SIF Y-B replay：无源码/包覆盖、NFD 与全部子进程在镜像
  内、终端结果与 oracle 一致。验收：integration（SIF 边界验证 +
  replay 结果摘要）；证据 `evidence/t010-exact-sif-replay-current.md`。

- **T011 — S5 Single Tiger Y-B Submission — TRANSFERRED**。实验机器通过 host-NFD node-local
  launcher（Spec 180 修订 122 暴露的设计缺口：Tiger 节点无 MiniNDN
  基底）提交一次 `yolo-functional`：一节点、一 RTX、四 Provider
  进程、一次 cold Y-B、三模型角色 CUDA 证据 + Merge CPU；无参数
  漂移的 byte-identical 重提仅限记录的 Slurm/host 入口前失败一次。
  文件：Spec 180 的 `packaging/ndnsf-di-container/jobs/spec180/*` 与
  `scripts/` 工具链（路径沿用）。验收：一次提交的完整结构化证据
  （协议/数值/设备/子进程退出/清理 oracle 全通过）；证据
  `evidence/t011-tiger-submission-current.md`。

移交任务的 evidence 路径是接收端应维护的记录名称，不表示本机已
生成或必须先生成；后续可在接收端 Spec 中登记实际位置并返回引用。

## Dependencies & Execution Order

```text
R001/R002/R003/R004（历史 safeguard；对应任务持续定向回归）
T001 -> T002 -> T003 -> T006
T004 为独立修复，但同样是 T007 的前提
T001/T002/T003/T004/T006 -> T007 PASS -> T005 -> T008
T008 -> T009 -> T012
T010/T011 = TRANSFERRED（外部实验工作，不是本地依赖）
```

R0 的失败关闭语义在定向修复期间保持；不得因 helper 存在或常量翻转
声称生产完成。只有第一个未关门是活动门；T007 未 PASS 前不运行完整
矩阵或资格套件。本地 G3/G4 为交付与关闭，须在 G0--G2 全通过后执行。
原 T010/T011 移交导致活动 ID 不连续；保持 ID 稳定，不重编号覆盖历史。
任何行为影响面变更使下游证据失效并回到最早失效门。
