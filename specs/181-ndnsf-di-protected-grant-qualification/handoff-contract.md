# Development and Experiment Handoff

**Revision**: 1 | **Date**: 2026-09-06 | **Status**: current
**Authority**: Spec181 revision 7, FR-008/009/010, SC-005/006.

## Ownership

所有者确认长期按开发/实验分工；Qwen/YOLO 只是模型案例。

| Responsibility | Owner | Deliverable |
|---|---|---|
| 代码开发、公共机制、缺陷修复 | 本机开发 owner | 明确 commit 与变更说明 |
| unit、真实 integration、本地 MiniNDN | 本机开发 owner | 同源输入/环境身份、完整结果与失败记录 |
| SIF 构建与 exact-SIF replay | 实验机器 owner | 引用开发交付身份的新构建/镜像身份与原 T010 验收 |
| TigerCluster 脚本、配置、实验执行 | 实验机器 owner | 原 T011 协议/数值/设备/退出/清理证据 |
| 实验发现反馈与脚本变更 | 实验机器 owner → 本机开发 owner | 绑定版本的复现材料与脚本/config commit |

本地交付只要求材料就绪，状态为 `READY_FOR_EXPERIMENT_MACHINE`；
不自动传送文件、写远端仓库、触发构建/实验，或声称接收端已签收。
当前开发完成后才另行讨论 Git 合并；此契约不授权合并。

## Local Delivery Record

T009 生成 `evidence/development-delivery.json`，schema 为
`spec181-development-delivery-v1`。以下为必填语义，不是当前交付实例。

| Field | Required meaning |
|---|---|
| `sourceRevision` | 本地审计与 T008 实际验证的 40-hex commit；无未记录源码差异 |
| `deliveryDigest` | 除该字段外的规范 JSON 摘要；文件按实际 SHA-256 绑定 |
| `inputs` | 源/契约/注册表/权威公钥摘要、模型与输入、oracle、runner、测试清单、parity 向量的引用与摘要 |
| `localEnvironment` | 实际本地解释器、依赖、native 构建/runtime、有效配置及复现所需非秘密变量的身份与记录引用 |
| `validation` | T007 PASS、T005/T008 完整本地结果引用及摘要；每项绑定同一源与其配置，保留失败与全部子进程/清理记录 |
| `reproduction` | 获取代码/依赖/外部工件及执行本地检查的完整命令、顺序、前提、预期 oracle 与输出路径规则 |
| `limitations` | CPU/YOLO 功能范围、已接受安全延期、未纳入交付的工作区改动及其影响；不能将未验收实现隐藏为限制 |
| `experimentTransfer` | T010/T011 的 TRANSFERRED 状态、实验机器 owner、原验收与反馈契约引用 |
| `handoffStatus` | `READY_FOR_EXPERIMENT_MACHINE`；与远端签收、SIF 或 Tiger PASS 分离 |

引用必须可定位并可校验，外部工件提供获取方式与摘要；不能依赖只有
本机临时目录才存在的唯一证据副本。秘密、私钥、明文能力值不得入库。
开发交付摘要固定本地证据，实验机器新增 SIF/集群身份时引用它，
不重新计算旧交付摘要来冒充同一次验证。

## Transferred Acceptance

| Original item | Disposition | External acceptance retained |
|---|---|---|
| T010 / original FR-008 SIF clause / original SC-005 replay | TRANSFERRED | 从交付 commit 进行容器原生构建；无 host `.so`/venv 注入；固定 SIF 哈希、镜像内依赖/NFD/子进程、protected Y-B replay 与 oracle、全部退出/清理 |
| T011 / original FR-009 / original SC-005 Tiger | TRANSFERRED | 既有 host-NFD node-local launcher；一节点一 RTX、四 Provider、一次 cold Y-B，三模型角色 CUDA 与 Merge CPU，协议/数值/设备/退出/清理完整；入口前基础设施失败最多一次同字节重提并保留原失败 |

实验机器维护后续实验计划和实际证据位置。本机不等待其排期、构建、
运行或签收才关闭 Spec181，也不能把 TRANSFERRED 项改为 PASS。
Spec180 冻结的镜像构建/运行边界继续控制后续实验，原文不改写。

## Feedback Contract

每次问题反馈至少携带：实际 `sourceRevision`、`deliveryDigest`、
实验脚本/config commit、SIF SHA-256（若已构建）、run-id、命令与
非秘密有效配置、首个失败边界、预期/实际结果、日志与结果摘要、
复现条件、全部子进程退出及清理情况。未产生的字段明确标为不适用。

实验脚本/config 变更提交回同一代码库；本机依据该版本复现并修复，
以新 commit 和受影响检查结果交付。若修改模型执行/协议逻辑，记录
行为变化并由开发 owner 验证，不能作为无影响的部署改动处理。

## Local Completion

T001--T009 的适用本地验收全部通过，T012 核对 FR/SC/输入/证据
映射后，才可发出唯一 `LOCAL_DEVELOPMENT_PASS`。活动任务共 10 个
（T001--T009、T012）；T010/T011 单独登记为 2 个移交项。
范围调整本身不完成任何原有本地任务，也不使旧 PASS 获得新源资格。
