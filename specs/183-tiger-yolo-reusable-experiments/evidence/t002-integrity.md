# T002 Content Integrity Checkpoint

**Date**: 2026-09-06
**Status**: PARTIAL_IMPLEMENTATION / FOCUSED_TESTS_PASS / runtime NOT_RUN

## Implemented

`Experiments/TigerCluster/runtime/yolo_profile.py`新增`check_plane`和`check_chain`：检查相对文件路径、大小/hash、schema、JSON歧义和I→R→E前驱绑定；输入阶段无需未来runtime文件。重新读取所有祖先，不接受过期的parent ID；文件搬迁不改ID，行为参数变化会改ID。结果明确区分完整性VERIFIED与资格NOT_EVALUATED。

普通文件以nonblocking/no-follow方式打开并检查stat，拒绝FIFO等特殊输入；hash前后检查inode/size/mtime/ctime以发现检查期间变化。此机制不替代部署后的重新校验或不可变bundle；不声称防住拥有目录写权限的恶意本机管理员。

## Focused Red / Green Evidence

1. 首个tracer bullet因模块不存在失败，新增输入阶段实现后1 passed。
2. 非法清单矩阵7 failed/3 passed，暴露缺文件、未知字段、绝对/越界/链接路径、空清单和错误parameters未拒绝；补严格校验后10 passed。
3. 身份链测试因check_chain缺失失败（1 failed/19 passed）；实现重新计算祖先后20 passed。
4. FIFO manifest测试在独立Python进程2秒超时；改成nonblocking普通文件检查后通过。
5. 最终命令：`python3 -m pytest -q Experiments/TigerCluster/tests/test_yolo_closure.py --tb=short --junitxml=Experiments/TigerCluster/results/spec183-t002-integrity-r1/junit.xml`，exit0，**21 passed in 0.36s**。JUnit为本地忽略输出，不进Git。
6. 复审将hash读取限制在声明文件大小+1字节，避免持续增长文件延长检查；同一focused命令使用独立r2目录复跑，**21 passed in 0.42s**，r1证据未覆盖。

21项全部是小型本地fixture，不使用模型/SIF/网络，也不是T008正式unit gate。范围包括未来文件缺省、非法inventory、旧parent/缺阶段、搬迁、行为参数绑定、同大小篡改、删除、错hash、额外行字段、重复JSON/NaN、数据及manifest特殊文件。

## Remaining Before T002 Can Close

- 在既有builder增加显式Spec183 host-gate dispatch，保持旧Spec175合同和source/ABI门。已实现：`--workload-kind spec183-yolo` 只接受 `--spec183-host-gate`，先绑定实际源seal/三类 case/evidence，再进入 Apptainer；Spec175 仍走原 host-gate/preflight 路径。
- 解析sourceLock/sourceSeal/wheels及所有harness/model文件的传递完整性，复用已有validator；当前最低文件集合不是完整闭包证明。
- 真实receipt内容/来源/数值/生命周期验证，拒绝仅手填PASS。
- 真实check/prepare/build/upload/submit边界接线和零外部调用mutation tests；当前新增builder负例覆盖了无效Spec183 receipt在版本探测、build前的零Apptainer调用，但完整生产入口仍未闭合，不能据此称生产bundle已资格化。
- 最终profile字段消费/受控env和冻结bundle执行时再检查。

## Spec183 dispatch checkpoint (2026-09-07)

新增 `packaging/ndnsf-di-container/lib/spec183_yolo_host_gate.py`，对
`tiger-yolo-host-minindn-manifest-v1` 做 fail-closed 校验：固定 workload、应用名、四 Provider、shared-backbone 图、normal/permission-rejection/negative-dependency 三 case、源 seal digest/revision，以及每个 retained evidence 文件的大小/hash/路径/no-follow 绑定。返回值明确标为
`YOLO_HOST_GATE_COMPONENT_ONLY`，不把 fixture 变成真实 MiniNDN 资格。

`build-local-sif.sh` 现在显式区分 Spec175 与 Spec183 dispatch；Spec183
receipt 在任何 Apptainer 调用（包括 version）前验证，旧 Spec175 version
命令顺序保持不变。负例测试确认错误 schema/legacy 参数时调用数为零。

聚焦命令：
`python3 -m pytest -q tests/python/test_spec183_yolo_host_gate.py tests/python/test_spec183_yolo_build_dispatch.py tests/python/test_build_local_sif_record.py --tb=short`
结果 **21 passed in 6.31s**。这仍是本地 receipt/builder 边界证据；真实
T010 MiniNDN receipt、T011 完整 SIF、T007 生产审计尚未完成，因此 T002
保持 unchecked，正式集群资格不开放。

将该 dispatch 与既有 TigerCluster/Spec183 focused selectors 合并复跑，
结果 **873 passed in 51.47s**（JUnit：
`results/t002-yolo-dispatch-r2/full-junit.xml`）；同样不包含 native/SIF/
MiniNDN/Tiger 实际运行。

预实施结构审计18 FR/6 SC/17 tasks、覆盖18 FR，PASS；既有接口缺口已分派T002/T005，未改变目标。无新架构未决，允许focused实现；完整生产收敛T007仍未完成。T002保持unchecked，正式集群资格不开放。
