# T003 Bounded Lifecycle And Identity Layout Checkpoint

Date: 2026-09-06
Status: PARTIAL_IMPLEMENTATION / FOCUSED_TESTS_PASS / runtime NOT_RUN

## Implemented And Wired

- `runtime/baseline.py::Processes.close(seconds=30)`用一个monotonic截止时间管理整组进程，反向停止、保留回收时间；不再为每个进程重新提供5+5秒预算。即使某组signal报错也继续处理其他组。未回收leader保留在owner中，并报告`cleanupTimedOut`/`cleanupError`，不伪造reaped。默认仍先向Apptainer leader发TERM，避免提前拆除FUSE挂载。
- `runtime/worker.py::run_finite_application`改用同一`Processes`实现，不再维护另一份Popen/TERM/KILL逻辑；增加显式cwd、cleanup预算，保留原timeout/exit异常以及finite与长期service区别。执行deadline和cleanup预算必须为有限正数，启动前拒绝None/NaN/Inf/bool/非正值。
- `runtime/identities.py::validate_role_homes`检查已准备的HOME、PIB及TPM文件布局，拒绝目录嵌套/链接、PIB或私钥hardlink共享、缺文件和特殊文件。只检查路径/inode，不读取私钥字节，不证明证书/身份正确，也不代替并发PIB lease。既有`issue`已在公开证书导入前调用此检查；真实SIF issuer尚未重跑。
- 身份模块增加deferred annotations，解决本机Python3.8读取`list[Path]`时的导入TypeError；未改变SIF Python版本或证书生成算法。

## Focused Evidence

红绿步骤：整组budget接口缺失1 failed；身份模块先因Python版本注解报collection error，修复后布局接口缺失2 failed；finite cwd接口缺失1 failed；无界deadline矩阵6 failed；跨角色私钥hardlink未拒绝1 failed。逐项修复后回归如下，旧证据未覆盖。

```text
python3 -m pytest -q \
  Experiments/TigerCluster/tests/test_worker_lifecycle.py \
  Experiments/TigerCluster/tests/test_process_group_budget.py \
  Experiments/TigerCluster/tests/test_yolo_identities.py \
  Experiments/TigerCluster/tests/test_yolo_runtime.py \
  Experiments/TigerCluster/tests/test_baseline.py \
  --tb=short --junitxml=Experiments/TigerCluster/results/spec183-t003-lifecycle-r4/junit.xml
```

exit0，**115 passed in 4.00s**。之前独立r1/r2/r3分别104/105/111项通过；JUnit为本地忽略输出。真实短进程测试覆盖四个拒绝TERM的service共用0.4秒预算、signal-denial时其余组照常回收且保留异常owner、正常退出/非零退出/超时、leader退出后的存活后代、显式cwd、自定义日志，以及不碰无关进程。signal denial只在OS调用边界注入；其余启动/等待/kill/reap实际执行。

身份layout fixture使用明确的非密钥测试字节，不执行真实NDN证书或权限流程；不能替代SIF issuer/跨角色授权测试。语法测试不能证明实际GPU已被绑定。用户定义的正式unit→integration→MiniNDN→SIF→Tiger门均未提前开放。

## Remaining Before T003 Closure

仍需YOLO四角色worker、PIB并发lease/identity验证、port/peer readiness的run绑定、实际profile字段消费、GPU allocation映射，以及整个worker退出时将同一预算传给相关owner。共享close已受限，不等于所有生产路径自动遵守整组预算。内核不可终止进程只能报告未回收并由作业边界处理，不能保证用户态总能杀死它。T002/T003仍unchecked，T007未通过，不提交Slurm。

Context Mode active health通过且文件来源匹配；CodeGraph已sync；Spec Kit按当前任务推进。GSD旧STATE不属于Spec183，继续以本Spec tasks/evidence及repository handoff保存状态。本轮是实现回归，不新增ARS统计/性能结论。
