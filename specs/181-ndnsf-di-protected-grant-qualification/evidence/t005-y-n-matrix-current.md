# T005 — Y-N 全矩阵语义重跑（MiniNDN）

**Status**: PASS (T005 R19, 2026-09-06; source ce6a4ba0)
**Layer**: implemented / wired / executed (same-source MiniNDN Y-N matrix)

## Current R19 Qualification

在 [native identity and convergence PASS](t005-exact-data-wire-repair-20260906.md#native-identity-r1-and-convergence-review)
之后，以 `ce6a4ba0f07bbdbc954a667f7846e5348c8861da` 干净隔离源码运行
维护 CLI `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-N`。
原始目录为 ignored workspace temporary directory 下
`spec181-t005-formal-20260906-r19/`，命令
`sudo -n /usr/bin/python3 <R19>/launch.py`。只启动一次，无子用例重试。
父 launcher 和维护 runner 均 exit 0；源码、输入前后摘要一致。

| Subcase | Actual result | Registered boundary / reason |
|---|---|---|
| Y-N-O | PASS / CONTROL | TERMINAL_RESPONSE / TERMINAL_RESPONSE_VERIFIED |
| Y-N-C | PASS / FAIL_CLOSED | PLACEMENT_DECISION / NO_FEASIBLE_CANDIDATE |
| Y-N-P | PASS / FAIL_CLOSED | ACK_CLOSED / ACK_PROVENANCE_REJECTED |
| Y-N-R | PASS / FAIL_CLOSED | PLAN_SEALED / ROLE_KIND_REJECTED |
| Y-N-I | PASS / FAIL_CLOSED | PROVIDER_EXECUTION_STARTED / NON_INGRESS_INPUT_REJECTED；实际 errorCode 为 DI_INPUT_FETCH_ROLE_MISMATCH |
| Y-N-E | PASS / FAIL_CLOSED | PROVIDER_GRANT_VERIFICATION / DI_PROTECTED_GRANT_REJECTED |
| Y-N-L | PASS / FAIL_CLOSED | EVIDENCE_ACCEPTANCE / REDACTION_REJECTED |

E 的 EXPIRED、WRONG_RECIPIENT、FORGED_AUTHORITY 三次独立运行全部
由选定 BackboneNeck Provider 的真实 verifier 在 BEFORE_ASSEMBLY
拒绝；分别为 expired、content-key envelope authentication、authority
signature 错误。证据绑定 request/attempt/plan/grant/provider，未用
User probe 或其他异常替代。总计九次运行、63 个应用子进程，全部
收集退出：control User 为 0，八个拒绝 User 为约定的 91；其他进程
按显式 terminationRequested 受控退出。复核无存活记录 PID、无 NFD、
无 cleanup-error、共享 state 目录无残留文件。

O 的真实数值 oracle matched=true，shape=[1,50,6]，maxAbsError=
0.0005340576171875，atol=0.001、rtol=0.0001。这是 CPU 功能结果，
没有性能含义。按既定 runner 契约，O/C/P/R/I/L 使用 plaintext-v1
控制环境，E 使用受保护纪元；O/I 保留的普通模型缓存不冒充受保护
存储结果，E 三目录无装配模型残留。受保护正向 Y-B 仍由 T008 验收。

已保存可入库的原始结果字段、63 个退出记录、三个 Provider 拒绝
记录、数值结果与 68 个原始文件摘要：
[R19 machine-readable evidence](t005-r19-matrix-result.json)。秘密及大日志
仍留在原始目录；本记录不宣称进程结束后的内存取证或远端资格。

T005 在此源身份下完成。T008 必须在最终交付源码上重跑完整
Y-A/Y-B/Y-N 和完整测试清单，不能把此单独矩阵替代总 gate。
下一步见 [T008 preflight review](t008-local-suite-preflight-20260906.md)。

## Historical Evidence Boundary

下文 2026-09-05 的 6/7、7/7 与不同提交追加记录均保留为历史诊断，
不构成 R19 的证据。旧重试入口仍停用；R1–R18 的失败均未覆盖。

Date: 2026-09-05. Source HEAD: `2f835386` 起（含 drain 修复 `28a91f47`）。

## Historical Diagnostic Sequence

修复链（全部提交）：
1. **drain 位置**（`28a91f47`）：runControllerLoop 内同步
   processEvents(2000ms) → controller readiness 达成。
2. **ContentStore 污染**（`3433c805`）：readiness probe 的 PUBPARAMS
   Data 被 NAC 的 CanBePrefix Interest 匹配 → abeType 解析
   "readiness" 失败 → publication ServiceUser 构造崩溃。修复：
   publication 前等待 freshness 窗口（6 s）过期。
3. **repo SIGINT**（`3433c805` + `5325165e` 的 user 部分 + 事件式
   handler）：native run 循环（GIL 释放）不响应 SIGINT → 清理超时
   SIGKILL(-9) 判负。修复：worker 线程跑 native 循环 + 主线程
   event 等待 + 信号 handler 只 set event。
4. **Y-N-I user 退出**：provider 的 DI_INPUT_FETCH_ROLE_MISMATCH
   拒绝证据由 runner 从 provider marker 验证，failure Response 不
   走 DATA_V1 通道——user 的 response 等待永不返回（r36 同款）。修复：
   user 的 Y-N-I 分支等待上限 3 s（低于 runner 的 5 s cleanup 窗口）
   后退出 91，判定由 runner 的 marker 门承担。
5. **瞬态 face 竞态**（r39 同类）：controller 启动时 NFD/face
   transport 竞态——`scripts/run_spec181_y_n_matrix_retry.py` 逐子
   用例隔离 + 最多 3 次重试。

驱动 3 结果（2026-09-05，`8abfc59d` 源）：

```text
Y-N-O PASS (attempts=3)
Y-N-C PASS (attempts=2)
Y-N-P PASS (attempts=2)
Y-N-R PASS (attempts=2)
Y-N-E PASS (attempts=1)   # 真实 grant 变异，live MiniNDN 一次通过
Y-N-L PASS (attempts=3)
Y-N-I PASS (attempts=1, FAIL_CLOSED)  # 3 s 响应上界 + runner marker 门
```

**历史跨运行汇总声称 7/7（不能晋升资格）**。Y-N-I 重跑成功依赖最后一项修复：
publishSignedAppData 的 face.put（CS 放置，f21d0665）在 controller
启动的 face 层引发 libndn-cxx segfault（dmesg 可见、0 字节 controller
日志）——回滚（b4fb1d6d）后 Y-N-I 一次通过。grant 的跨节点 fetch 由
identity 前缀路由（c0a887fe）+ forwarding hint（1232a043）承担，
不再需要 CS 放置。

三次矩阵尝试的失败链与修复：

### 尝试 1（drain 修复前）

全部七子用例 `CASE_RUNTIME_PROCESS_START_FAILED:control`；controller
进程 `ServiceController readiness timeout`（15 s 等待内 readiness
probe 未完成）。根因：Spec 180 r36-r42 的 Controller 启动 drain 修复
（`processEvents` 前后排空）从未提交到本分支，face transport 竞态
重现（r39 的同一 Y-N-I 现象，扩大到全部子用例）。

### 尝试 2（keepRunning drain 放在 ServiceController::start()）

全部七子用例 controller 进程 **SIGSEGV**（returncode=-11）。根因：
`processEvents(1000ms, keepRunning=true)` 启动后台 io 线程，与
readiness probe 循环的 `io.run_for`（同一 io_context、另一线程）并发
——未定义行为。这是与 r36-r42 位置不同的实现错误（r36-r42 的确切
drain 位置未提交，本分支重新推导时先踩了这个坑）。

### 尝试 3（同步 drain 移到 runControllerLoop，start() 之前）

- controller 达成 `SPEC180_CONTROLLER_READY`（readiness probe 成功，
  drain 修复方向正确）;
- 但 controller 进程在 runtime-publication ServiceUser 构造时崩溃：
  `Fetched public parameters cannot be authenticated: Validator/policy
  did not invoke success or failure callback`（NAC-ABE PUBPARAMS 的
  验证器失败，boost terminate）;
- DEBUG 日志证据（2026-09-05 尝试 3 的 Y-N-O）：
  - `setting InterestFilter: /example/controller/PUBPARAMS` 存在（AA
    filter 就位，readiness probe 因此成功）;
  - `Request public parameters (attempt 1): /example/controller/
    PUBPARAMS` 出现**两次**（两个 NAC consumer 实例各自 fetch）;
  - NFD 日志（memphis）PUBPARAMS 流量为 **0**——Interest 未出本地
    face，Data 由**本地 IMS 满足**;
  - `Start validating data` 从未打印——验证器**从未开始**验证;
  - 失败回调在 fetch 后约 100 ms 内触发（非超时、非 NACK 重试）。
- 根因假设（待验证）：runtime-publication ServiceUser 构造时（
  `user.start()` 之前）face 302 的 io 尚未运行，NAC consumer 的
  Interest 与验证器的证书 fetch 都在未运行的 io 上排队；本地 IMS
  的 PUBPARAMS 满足使 Data 立即到达，而验证路径的证书请求无法在
  构造返回前完成，state 随构造异常析构（"did not invoke" 是析构
  连锁）。r39 时代的成功运行依赖其未提交工作树中与此不同的
  构造/启动时序。

## 已知环境事实

- 输入：Spec 180 的 canonical YOLO26n 包（本地候选目录
  输入 bundle r115 所在）、Y-N 输入 bundle r115、注册表
  `specs/180/contracts/trust-root-registry-v1.json`;
- 命令：`unshare -Urnm python3 Experiments/NDNSF_DI_YoloAckDriven_Minindn.py
  --case Y-N`;
- 与 r39（6/7 PASS）的差异：r39 运行在 Spec 180 关闭前的未提交
  工作树（keepRunning drain + 诊断 marker）;本分支继承的是其
  **提交前**的基线（8b24911b），r36-r42 的修复未随分支迁移。

## 单元/进程内层（已执行，独立于矩阵）

Y-N-E 真实变异 probe 与 runner 语义的 143 项测试全绿（见
`evidence/t006-y-n-e-grant-mutation-current.md`）——矩阵失败发生在
Controller/NAC 启动层，不影响已锁定的 verifier 语义。

## Verdict

BLOCK / NOT PROVEN（当前完整矩阵未获资格）。首个未完成门是 G0，
先修复 native 生产路径、真实变异与重试证据边界，再取得 T007 PASS。
后续失败记录必须使用新 run-id，不覆盖这里的历史诊断。
