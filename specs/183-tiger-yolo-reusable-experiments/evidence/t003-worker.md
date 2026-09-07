# T003 Role Runtime Implementation Acceptance

Date: 2026-09-06
Status: IMPLEMENTATION_AND_FOCUSED_GATE_COMPLETE / runtime qualification NOT_RUN

## Task Boundary And Coverage

T003实现可供T004/T005使用的节点角色运行组件，不声称已完成operator入口、YOLO业务参数生成、实际证书签发或SIF/GPU资格。原先两个partial checkpoint见`t003-launch.md`和`t003-lifecycle.md`；本checkpoint关闭T003代码及聚焦验收，后续生产接线仍由T004/T005和T007负责。

| T003 requirement | Implementation and evidence |
| --- | --- |
| Preserve CPU behavior | 既有`test_baseline.py`、finite/cleanup回归仍通过；原issuer不传角色map时保持原CPU身份与peer列表 |
| Role/GPU/mount/cwd mapping | `NodeRuntime`固定A/B启动角色，CPU/单节点case包含四Provider；调用原`container_command`，为模型角色显式传GPU和各自只读artifact projection；Merge不接GPU/model mount；固定`/bundle`工作目录 |
| Identity/PIB isolation | `identity_inventory`支持生成的YOLO role→identity map；原`issue`逐角色签发并只分发peer公开证书；`validate_role_homes`检查PIB/TPM别名；`RoleHomeLease`用真实flock阻止合作worker并发使用同HOME |
| Owned process groups and cleanup | `NodeRuntime`调用共享`Processes`，同一cleanup预算；组仍存在时保留HOME lease，记录`leaseReleased`；清理期间忽略第二次TERM/INT并恢复原handler |
| Bounded readiness and peer checks | `wait_marker`有monotonic截止时间、64KiB增量读取、regular-file检查，始终检查子进程和peer failure；已退出进程留下的READY不能通过 |
| Wrong cwd/port/shared PIB/env | 拒绝缺目录、错误节点角色、NFD非法port、输出覆盖bundle或经symlink写入bundle；NFD启动调用原`nfd_config`，模型状态/ORT路径显式置于每角色`/output`；前两checkpoint覆盖宿主env/PIB/私钥共享 |
| No copied supervisor | 新worker只组合既有容器命令、identity和Processes；没有复制旧Spec180 supervisor或重写NDNSF协议/DI调度 |

`start_service`接收的是未来coordinator从冻结配置生成的argv，不是开放给操作者的任意命令入口；尚未提供可绕过T002资格的CLI。`start_forwarder`在持有NFD HOME lease后才生成配置并调用SIF内NFD路径。`identity_inventory`只允许实验生成的普通ASCII NDN name组件，排除URI别名；此限制不是NDNSF协议限制。role map是prepare产生的artifact，不能变成第二份手工配置。

## Focused Red / Green

新worker缺失1 failed；两个worker可同时打开同PIB的回归1 failed；marker接口缺失1 failed；NFD接口/port矩阵6 failed；issuer inventory接口缺失1 failed；缺cwd/mount或output覆盖bundle矩阵5 failed；role-output symlink越界1 failed；二次TERM中断清理的真实外层进程返回-15。逐项实现/修复后，最终命令为：

```text
python3 -m pytest -q \
  Experiments/TigerCluster/tests/test_yolo_worker.py \
  Experiments/TigerCluster/tests/test_yolo_identities.py \
  Experiments/TigerCluster/tests/test_worker_lifecycle.py \
  Experiments/TigerCluster/tests/test_process_group_budget.py \
  Experiments/TigerCluster/tests/test_yolo_runtime.py \
  Experiments/TigerCluster/tests/test_baseline.py \
  Experiments/TigerCluster/tests/test_yolo_closure.py \
  --tb=short --junitxml=Experiments/TigerCluster/results/spec183-t003-worker-r2/junit.xml
```

exit0，**171 passed in 5.87s**，包括27项新worker测试。r1为170 passed in 5.27s，未覆盖其后增加的二次信号修复；两次结果独立保留。外部Apptainer/NFD由明确fixture替代，但角色运行器、env/argv构造、Popen进程、flock、TERM/KILL/wait和跨进程锁竞争真实执行。这不是实际NFD网络、CUDA、NDNSF协议或YOLO数值结果。

## Mandatory Downstream Wiring

- T004从唯一profile生成完整冻结run plan、identity map、角色专属model projection；不能把含oracle的整个package挂给Provider。精确SIF/脚本/model/hash/原生库等仍要T002资格，当前NodeRuntime不验证这些资格。
- T005使用原ACK-driven User、native Provider和安全Controller/Repo；必须传`NDNSF_DI_STATE_ROOT=/output/state`，原生ORT证据使用`NDNSF_DI_ORT_PROFILE_PREFIX=/output/ort/session`；源码明确消费这些设置。signed publication/permission及NFD socket/route探测不能被日志marker代替。
- T005按请求调用one-shot User，Provider长期保留；保持独立request/attempt/输出，有限应用不调用`start_service('user', ...)`。全worker退出时的有限应用和service预算归coordinator统一管理，不能串联多个30秒预算。
- T007审计上述生产链真正接入后才允许正式unit/integration/MiniNDN/SIF/Tiger；不把T003完成记成环境资格。

Context Mode active health通过、CodeGraph已sync且确认`RoleHomeLease`调用者为`NodeRuntime._start_service`。GSD采用仓库handoff，不恢复旧Spec168状态。本轮不涉及ARS统计结论，没有构建、上传或Slurm提交。
