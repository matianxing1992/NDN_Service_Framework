# T006 Production Grant Mutation Repair

**Date**: 2026-09-05 | **Source baseline**: `f9e41ff2` + working tree
**Layer**: implemented / executed / measured（focused production integration） | **Status**: PASS

## First Boundary

现有 Y-N-E 在 User 的 canonical binding 中调用进程内 verifier 后
抛出固定异常，并允许 User 在 `ARTIFACTS_READY` 之前报告 PASS。
该路径没有将变异发布给选定 Provider，不满足 FR-004/T006。
新定向回归保存在忽略的工作区临时目录
`spec181-t006-production-20260905-r1/red.log`：8 项失败，分别证明
三种变异未进入实际发布 seam、变异配置/保护纪元缺失未拒绝，以及
User 异常/marker 仍可冒充 Provider 拒绝。

## Repair Direction

保留正常签发及 requester 校验，在实验入口截留待发布 grant，
校验正常返回后只变异一个实际选定 Provider 的记录，更新对应
grant 名/摘要并经既有签名 APP Data 发布。Provider 仍走未经替换的
精确获取、封印绑定、权威校验和信封解包；记录实际 verifier 的
结果及请求/attempt/Provider/计划绑定。只有该生产记录与实际发布
记录一致、拒绝原因符合指定变异、且所有子进程清理完成才可接受。

移除 User 进程内 probe 的资格用途；测试构造器本身不输出成功裁决。
三个变异及正向控制使用独立原始目录。T006/T007 当前均未闭合，
不运行正式全矩阵。

## Focused Regression Progress

发布与 oracle 修复后，r1 的首轮定向检查 47 PASS，r2 扩展至既有
negative-verdict/native-launch 接口后 62 PASS。r2 `runner.log` 中
87 PASS / 1 FAIL：旧测试仍要求进程内 Y-N-E probe 写 PASS，该期望
与本次修正的生产证据边界冲突。已将该测试改为要求拒绝进程内资格，
正式矩阵的 Y-N-E 分派则要求三个独立变异目录，全部通过才聚合。
native 构建与真实网络验证仍在进行；T006 尚未勾选。

r2 的更新后 Python 定向组为 150 PASS。独立 C++ harness 编译止于
链接：沿用早期命令时漏列后来新增的 store translation unit，
出现 `sealNativeAssembledEntry` 等 undefined reference；原记录保留
在 `focused-build.log`。这是 harness source-list 错误，不是运行时
回归。下一次使用新目录补齐实际依赖；统一 native 构建仍沿用正在
运行的同一进程，不因该独立 harness 失败而重启。

r3 `focused-build.log` 为命令路径错误：误写了不存在的
`NativeProtectedStore.cpp`，编译器没有启动。CodeGraph 尚未收录
新符号，精确源码检索确认实际路径为
`cpp/ndnsf-di/NativeProtectedArtifactStore.cpp`；r4 使用该文件。
统一 native 构建已 exit 0，独立 harness 的失败不作协议结果。

r4 harness 编译/运行 PASS（22 cases，含 runtime、grant 及 verifier
三个测试文件）。r5 首次 EXPIRED 网络入口在
配置预检以 `CASE_CONFIG_ROLE_SET_INVALID:Y-N` 退出；复用的 Y-B
配置只有四种角色，Y-N 还要求 FullModel，未创建网络。已核实既有
`spec180-yolo-y-n-inputs-r115` 包含完整五角色配置及现存拓扑；后续
负例使用该 Y-N 输入并显式启用保护纪元，正向控制仍用 Y-B 输入。

## First Live Results and Positive-control Failure

r9 EXPIRED、r6 WRONG_RECIPIENT、r7 FORGED_AUTHORITY 均经真实发布和
精确获取，在 native BackboneNeck Provider 被对应 verifier 原因拒绝，
请求、attempt、计划及 grant 摘要一致。三次清理后 exit 0；目标
Provider 没有装配工件。详细判据在各目录的
`case/grant-rejection-evidence.json` 与 `case/negative-evidence.json`。

r8 正向控制 UNQUALIFIED：四个 grant 都 VERIFIED，随后 Merge 在
Selection 后约 10 s 获取 DetectShard0 的 tensor manifest 失败，
User 最终超时。此时 DetectShard0 的 ORT session profile 才于
17:46:44.196 创建，Merge 失败日志为 17:46:44.479；未证明有效输入
在该窗口内可用。第一失败边界是冷装配期间的依赖等待，不能计为
grant 拒绝或正向成功。

源码核对：`NdnsfCollaborationDependencyIo.cpp` 精确获取上界为
`min(remaining hard deadline, edge.noProgressDeadlineMs, m_fetchTimeoutMs)`；
User 把 no-progress 固定为 10000 ms，而每次保护执行需要先装配和
加载冷模型。下一修复将此等待绑定到调用者已经配置的请求预算，
保留既有 hard deadline 和取消边界，不再用较短的隐式常数误报。
随后使用新的独立目录复验正向控制及三个变异；T006 仍保持 OPEN。

## Final Acceptance

最终配置中，no-progress 使用维护 User 命令的请求预算（60000 ms），
native 精确获取仍取其自身上界与剩余 hard deadline 的最小值，并检查
取消。r10 计时记录显示 Merge 的一次 dependency fetch 为 13974 ms，
从 PLAN_SEALED 到 TERMINAL_RESPONSE 为 14.208 s；这是单次功能
观察，不是性能资格结论，但证明先前 10 s 窗口会提前截断有效路径。

| Run | Input and expected boundary | Actual result |
|---|---|---|
| r10 | protected Y-B positive control | PASS / exit 0；4 grant VERIFIED，3 ORT CPU + native Merge，terminal payload 1267 bytes |
| r11 | EXPIRED；实际 Provider 授权边界 | PASS / exit 0；`key grant is expired` |
| r12 | WRONG_RECIPIENT；实际 Provider 解包边界 | PASS / exit 0；`content-key envelope failed authentication` |
| r13 | FORGED_AUTHORITY；实际 Provider 权威校验边界 | PASS / exit 0；`key grant authority signature is invalid` |

三个负例的 Provider 均为 `/example/provider/BackboneNeck`，请求分别为
`/spec180-y-n-e-107fe727e98a2c50`、`/spec180-y-n-e-df23c4ae1b7556b0`、
`/spec180-y-n-e-b63ceda3f44a64a1`，均为 `attempt-1`。collector 核对
实际发布与 verifier 记录的 request/attempt/provider、planCoreDigest、
grantDigest，并将 planDigest 与真实 PLAN_SEALED 日志绑定；User
异常、旧 marker、其他配置/超时错误都不能证明这组拒绝。

每次收集 7 个子进程；负例 User 自行 exit 91，其余进程由维护清理
流程请求 SIGINT 后退出（-2/130），全部状态均有记录，不能说成全部
子进程 exit 0。目标 Provider 的 artifact cache 没有文件；四个最终
运行的 native cache 无 ONNX 明文/weights.bin。正向控制保留三份
`model.onnx.cipher`，无明文模型。原有 5 个 NFD PID/PPID 保持不变。

统一构建 manifest SHA-256：
`6fc448391420dc59bd4943129adfd1c0e8bcc7a7df9f1e001be9b93cc33610e5`。
来源为 `f9e41ff2` 加本轮工作区实现；T002 原有 factory/assembler
工作区接线也参与实际运行，不能宣称这是干净提交候选或正式资格。
源码路径和运行参数见对应 `run.sh`；日志均保留在上述独立目录。

定向回归：r4 `all-focused-final.log` 为 153 Python PASS，
`runtime.log` 为 22 C++ PASS，`rebuilt-parity.log` 为 3 PASS；
新增 oracle 包含错误身份、计划/摘要、无关拒绝、单目标变异、已有
证据不可覆盖、变异失败阻断聚合等检查。r1/r2/r3/r5/r8 的失败保留。

T006 定向验收闭合，A04 CLOSED；T007 仍 BLOCK。T005 重试器、正式
同源矩阵、T001/T002 全部生产验收与 T003 独立装配 parity 继续开放。
