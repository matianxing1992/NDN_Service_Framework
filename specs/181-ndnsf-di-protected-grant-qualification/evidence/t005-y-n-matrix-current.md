# T005 — Y-N 全矩阵语义重跑（MiniNDN）

> **Current status: BLOCK / NOT PROVEN (2026-09-05)**。下文 6/7、7/7 和不同提交的追加记录均为历史诊断，不构成同源矩阵。T006 后续真实 Provider 变异已定向验收，见 [生产修复](t006-production-repair-20260905.md)。旧重试入口已停用，维护矩阵首个失败即停止，见 [证据保留修复](t005-evidence-repair-20260905.md)。T007 新 PASS 前不得重跑完整矩阵；新 run 保留全部失败、唯一目录与源/配置身份。

**Layer**: implemented（runner 语义 + Y-N-E 真实变异 + 矩阵收集器
T006 吸收）;executed（2026-09-05 三次矩阵尝试，状态如下）;无
measured 声明。

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
