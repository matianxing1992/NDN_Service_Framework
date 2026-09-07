# T004 CLI And Submission Journal Checkpoint

Date: 2026-09-06
Status: PARTIAL / runtime qualification NOT_RUN

## Implemented

- 唯一 `jobs/yolo/submit.py check` 接入 profile loader 和 I/R/E 内容链；profile
  引用摘要在解析链前后核对。坏输入 exit2；当前内容匹配仍 exit78，不授予资格。
- 确定性 case/run 预览复用节点角色 owner，生成四 Provider 预期放置、身份名、
  独立请求 ID/输出、caseBehaviorDigest；真实 allocation 为 null，argv/mount/
  签名材料/合格候选明确 unresolved。不写文件，不伪造 Selection。
- SubmissionJournal 在预先存在的共享根下按 candidate/gate 原子登记，真实
  flock/原子 replace/fsync、保留旧 run。PREPARED 只有一个所有者，未知提交
  通过 exact comment/job 查询结果恢复，零匹配不重提，多匹配保持占用并报错。
- 仅 PREPARED 可以取消；SUBMITTING 及之后不得用取消/finish 绕过未知 job。
  正常终态必须绑定既有 jobId，不能覆盖旧 FAIL 为 PASS。

## Focused Evidence

缺 CLI、缺运行预览参数、缺 journal/reserve、缺状态转换、缺安全取消均先出现
实际 tracer failure 后实现。fresh-interpreter audit 捕获 Python3.8 jsonschema
经 uuid/platform 导入时执行 `uname -p`；这是只读标准库探测，不是构建/Slurm。
测试只允许该精确探测与 `/dev/null`，继续禁止所有实验子命令、网络及文件修改。
保留这一区别，不声称冷导入完全零 subprocess。

19 项 journal 测试包含两个独立进程 barrier 后竞争、进程在 SUBMITTING 后
直接退出并重建 journal、零/多/无关 query、jobId 类型与不匹配、旧终态保留、
安全取消、损坏记录、原子 replace 失败、目录 fsync 失败和 symlink 重定向。
损坏 state 类型和 RUNNING 缺 jobId 的测试暴露两项输入校验缺口，已修复。
16 项 CLI 测试实际从仓库外启动入口，验证四种 case、cwd/物理位置/新 run
摘要规则、参数错误、内容篡改和 Python audit 边界。fixture 全为合成小文件，
不用于宣称真实 SIF、GPU、NDN、密钥或 YOLO 数值正确。

最终命令：

```text
python3 -m pytest -q Experiments/TigerCluster/tests --tb=short \
  --junitxml=Experiments/TigerCluster/results/spec183-t004-cli-journal-r2/junit.xml
```

Exit0，**235 passed in 12.16s**（先前200 + CLI16 + journal19）。r1 为
234 passed in 12.57s，早于新增安全取消测试，原报告保留不覆盖。JUnit/raw
位于ignored results，未提交生成物。

## Remaining And Gates

T004 仍 unchecked：真实 enabled profile、prepare 不可变 bundle/role-only model
projection、所有实际 argv/env 消费、run.sbatch、Slurm query/submit/collect
生产接线尚未完成。journal 单测不是 Tiger 共享存储语义验证，T012 需实测；
finish 上层必须验证 allocation 已终止，journal 自身不发资格 receipt。
T002 专属 source/workload/receipt 校验依赖 T005/T006；不能放开 build/upload。
下一步完成冻结运行 bundle 和实际业务参数接线，再连上 journal 的外部命令边界。

Context active health通过，CodeGraph已先定位 owner；Spec Kit prerequisites及
requirements checklist 10/10通过。GSD仍degraded/W019，旧phase35不是Spec183
权威，使用tasks/handoff；本轮非统计实验，不新增ARS结果结论。无编译、SIF、
上传、Slurm作业或模型执行。

## 2026-09-07 local prepared-profile binding

当前源码审查发现 `_local` 比 `_submit` / `_collect` 少了 profile digest 比较：
它可能给 profile B 冻结的 prepared run 读取 profile A 的 hostMinindn receipt。
虽然 worker 仍未接线，此遗漏会在启用真实执行时造成跨配置证据混用。

新增测试在两个摘要不同的情况下禁止读取 gate receipt；修改前确实触发了
被禁止的调用（1 failed）。`_local` 现先比较 `report.documentDigest` 与
`prepared.profileDigest`，不一致立即报 `PROFILE_CHANGED_AFTER_PREPARE`。
既有 hostMinindn→localSif 顺序和零文件副作用行为保留。

```bash
python3 -m pytest -q Experiments/TigerCluster/tests/test_yolo_submit.py \
  --junitxml=Experiments/TigerCluster/results/spec183-local-profile-binding-20260907/focused.xml
```

**40 passed in 33.80s**；只跑受影响 CLI 文件，JUnit 保留在本地 ignored results。
这关闭的是 T004 的 profile 绑定遗漏，不关闭实际 local/run/staging 接线；
T004/T007 仍未验收，不代表 MiniNDN、SIF 或 GPU PASS。
