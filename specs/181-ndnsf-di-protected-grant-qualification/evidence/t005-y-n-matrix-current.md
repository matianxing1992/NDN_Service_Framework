# T005 — Y-N 全矩阵语义重跑（MiniNDN）

**Layer**: implemented（runner 语义 + Y-N-E 真实变异 + 矩阵收集器
T006 吸收）;executed（2026-09-05 三次矩阵尝试，状态如下）;无
measured 声明。

Date: 2026-09-05. Source HEAD: `2f835386` 起（含 drain 修复 `28a91f47`）。

## 状态：矩阵未完成（诚实记录，非 PASS）

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

NOT PROVEN（矩阵未通过）。修复方向已锁定（drain 位置 + NAC
pubparams 验证时序），后续 run 将在本文件追加 executed 记录。
