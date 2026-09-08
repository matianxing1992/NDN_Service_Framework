# R3 Chapter Revision Review

## Conclusion

R3 已修正本轮确认的事实错误，重写 23 组关键 API 说明，补充主体例子与五项目标。
旧 BC-01 至 BC-04 的有效内容合入相关 AC，删除重复补充章；当前/目标变为 58/63 章。
这是对 R2 逐章审阅的处理记录，不是所有公开函数已具备完整语义契约的声明。

## Substantive Corrections

- grant 的调用方输入是身份/服务/属性单项，完整策略由 Controller 内部重建；保留 pending rotation 例外。
- StreamPublisher::push 接收已签名 Data；writer.publish 返回 false 与 finish 抛异常分开。
- C++ Repo lookup 仅接受精确名；存储读取在目录锁外，不承诺全局一致快照。
- AC-18 改用实际生成、状态机、KV、journal 方法；区分 attempt/stream/context epoch。
- Qwen 终态 cancel 会抛异常；NativeInferenceHandle.cancel 的幂等行为不能套用。
- beginTurn 需要已登记父记录；abortTurn 当前无实际持久化/aborted 修改，rename 不等于已具 fsync 耐久性。
- Drone Execute 的 command/mavlink_hex、ACK 分支与 700 ms 等待预算明确；manual_control 仅确认转发。该 handler 未见 lease/readiness 独立重验，不把 GS 门禁外推到 Drone。
- 目标 R0 的过时声明删除；TG-01 至 TG-05 已接受的方向与未定稿线协议分开。

## Chapter Disposition

以下编号保留 R2 审阅 ID，便于逐条对照；REVISED 表示所列文档修改，KEEP 表示保留原用途。
不能按 REVISED 数量推断每个原建议或每个源方法均已完成运行/语义资格验收。

| R2 ID | 主题 | 处理与 R3 位置 |
|---|---|---|
| 1 | 文档范围与源码基线 | KEEP：保留概览/已清楚的分支；共同读者术语见 AC-01。 |
| 2 | 系统组成与职责边界 | KEEP：保留概览/已清楚的分支；共同读者术语见 AC-01。 |
| 3 | Core 角色、命名与启动 | REVISED：从配置到可调用服务；原章节 + 对应 AC。 |
| 4 | 普通与 Targeted 调用 | REVISED：应用怎样判断完成；原章节 + 对应 AC。 |
| 5 | 协作规划与选择事务 | REVISED：一个有依赖的协作计划；原章节 + 对应 AC。 |
| 6 | 对象、连续流与调用流 | REVISED：对应到代码类型；原章节 + 对应 AC。 |
| 7 | 能力、并发、持久状态与观测 | REVISED：一次执行槽位的生命周期；原章节 + 对应 AC。 |
| 8 | 身份与请求保密 | REVISED：按攻击对象选择检查；原章节 + 对应 AC。 |
| 9 | 授权版本与权威 | KEEP：保留概览/已清楚的分支；共同读者术语见 AC-01。 |
| 10 | 在线授予 | REVISED：单项输入与策略物化；原章节 + 对应 AC。 |
| 11 | 在线撤回 | KEEP：保留概览/已清楚的分支；共同读者术语见 AC-01。 |
| 12 | 版本发现 | KEEP：保留概览/已清楚的分支；共同读者术语见 AC-01。 |
| 13 | 授权变化影响 | CORRECTED：全局版本基线与 TG-01 分开，删除未采纳目标的过时说法。 |
| 14 | Repo 对象模型 | REVISED：对象引用的阅读顺序；原章节 + 对应 AC。 |
| 15 | Repo 持久化与可见性 | REVISED：成功信号属于哪个阶段；原章节 + 对应 AC。 |
| 16 | Repo 可恢复传输 | REVISED：断点下载的操作身份；原章节 + 对应 AC。 |
| 17 | Repo 目录与副本 | REVISED：一次目录读不等于副本验收；原章节 + 对应 AC。 |
| 18 | DI 入口和实现层次 | REVISED：如何选择入口并解释失败；原章节 + 对应 AC。 |
| 19 | DI 图、候选与放置 | REVISED：用两段模型读懂数据结构；原章节 + 对应 AC。 |
| 20 | DI 计划、授权与制品 | REVISED：五种摘要不互换；原章节 + 对应 AC。 |
| 21 | DI 角色执行与数据流 | REVISED：A 到 B 的实际执行；原章节 + 对应 AC。 |
| 22 | DI token、KV 与延续 | REVISED：一次生成与下一轮对话；原章节 + 对应 AC。 |
| 23 | DI 部署与扩展 | REVISED：管理动作与请求动作的区别；原章节 + 对应 AC。 |
| 24 | UAV 容器与界面 | REVISED：界面如何使用状态；原章节 + 对应 AC。 |
| 25 | 飞控与操作权 | REVISED：沿失败位置排查；原章节 + 对应 AC。 |
| 26 | 巡逻与补偿 | REVISED：两个分片的任务例子；原章节 + 对应 AC。 |
| 27 | 视频、遥测与录像 | REVISED：丢帧与录像完整性；原章节 + 对应 AC。 |
| 28 | 检测与多视角 | REVISED：两视角输入到终端接受；原章节 + 对应 AC。 |
| 29 | 跨模块完整路径 | REVISED：贯穿四模块的例子与实际接线边界；原章节 + 对应 AC。 |
| 30 | 构建配置部署 | REVISED：可复现入口与本轮命令；原章节 + 对应 AC。 |
| 31 | 实现状态与验证 | REVISED：能力、入口与证据的对应；原章节 + 对应 AC。 |
| 32 | Core 源码索引 | KEEP：保留概览/已清楚的分支；共同读者术语见 AC-01。 |
| 33 | Repo/DI 源码索引 | REVISED：生成与会话的准确文件入口；原章节 + 对应 AC。 |
| 34 | UAV/交付索引 | REVISED：索引的使用范围；原章节 + 对应 AC。 |
| 35 | 文档维护 | REVISED：章节的人工接受条件；原章节 + 对应 AC。 |
| 36 | API 阅读方法 | REVISED：AC-01；阅读路径与例子的前提、常用术语、一次查阅示例。 |
| 37 | Core 容器生命周期 | REVISED：AC-02；创建与所有权、调用次序、前置条件与失败。 |
| 38 | Core 请求/Targeted | REVISED：AC-03；请求标识与参数、最小调用示意、事件与接受边界、回调类型。 |
| 39 | Provider 作用域注册 | REVISED：AC-04；注册者必须保存句柄、调用与关闭示意、时序与资源。 |
| 40 | 延迟规划/提交 | REVISED：AC-05；两种时间预算、两角色例子、提交顺序与结果、不可越过的状态、CollaborationPlan 的具体字段。 |
| 41 | 调用流 API | REVISED：AC-06；如何取得 writer、返回值与异常、两事件示意。 |
| 42 | 对象/连续流 API | REVISED：AC-07；选择哪一种 API、连续流例子、资源与恢复。 |
| 43 | 授权 API | REVISED：AC-08；输入是单项授权、返回分支、接收者何时更新。 |
| 44 | 租约/观测/并发 API | REVISED：AC-09；租约携带什么、正常状态链、结果与诊断、并发与观察。 |
| 45 | Repo 对象/packet API | REVISED：AC-10；对象与 manifest、原始 packet 例子、存储与可见性。 |
| 46 | Repo 上传提交 API | REVISED：AC-11；公开上传调用、显式阶段、结果字段、提交与重试。 |
| 47 | Repo 下载状态机 API | REVISED：AC-12；kwargs 的实际含义、调用次序与结果、如何读统计、底层分段是另一层状态。 |
| 48 | Repo 目录/副本 API | REVISED：AC-13；本地目录操作矩阵、lookup 的实际失败与锁边界、副本判断例子。 |
| 49 | DI Python/native 请求 API | REVISED：AC-14；Python 与 C++ 是两个现存入口、原生输入与选项、句柄行为、等待例子。 |
| 50 | DI 模型/放置扩展 API | REVISED：AC-15；规划对象怎么连接、两段模型例子、扩展最小职责、当前源码的描述符与候选增补。 |
| 51 | DI 准备/发布/封存 API | REVISED：AC-16；阶段输入输出、发布改变身份的例子、失败边界。 |
| 52 | DI Provider 执行 API | REVISED：AC-17；组成一个 Provider、执行次序、grant 从哪里来、关闭与迟到工作。 |
| 53 | DI 生成/KV/会话 API | REVISED：AC-18；四个 owner 与真正执行入口、Config 与结果中的关键字段、Qwen 状态机：允许的调用、两 token 示例与替换、token 循环与重复抑制、KV 操作与资源状态、从第 N 轮延续到 N+1 轮、checkpoint 的三个步骤、journal 的实际限制。 |
| 54 | UAV 飞控 API | REVISED：AC-19；内部接口与线载荷、完整命令路径、三类状态的例子、扩展边界、当前 Execute 字段与 backend 返回。 |
| 55 | UAV 任务/报告 API | REVISED：AC-20；任务和调用各自存活多久、两个 part 的补偿例子、返回、恢复与资源。 |
| 56 | UAV 视频/遥测 API | REVISED：AC-21；队列输入与准确返回、popLatest 的例子、遥测值与时间、捕获到录像的分工。 |
| 57 | UAV 多视角扩展 API | REVISED：AC-22；输入对象与责任、两个视角的例子、算法扩展与错误。 |
| 58 | API 扩展检查 | REVISED：AC-23；新增服务的最小闭环、新增 DI 策略的最小闭环、设计维护的交付单元。 |
| 59 | BC-01 授权安装 | MERGED：授权输入、状态安装、失效与恢复合入第 10 章 / AC-08。 |
| 60 | BC-02 句柄等待/观察 | MERGED：句柄行为合入 AC-14；当前描述符进度移入 AC-15，冻结目标不复制。 |
| 61 | BC-03 精确目录查询 | MERGED：精确 lookup、异常与锁边界合入第 17 章 / AC-13。 |
| 62 | BC-04 UAV/覆盖 | MERGED：UAV 接口分别进入 AC-19—22；覆盖等级进入 AC-01 / MANAGEMENT。 |
| 63 | TG-01 授权影响范围 | REVISED：TG-01；补字段职责、对象连接/状态、兼容与验收；R3 第 59 章。 |
| 64 | TG-02 原生唯一 owner | REVISED：TG-02；补字段职责、对象连接/状态、兼容与验收；R3 第 60 章。 |
| 65 | TG-03 异步契约 | REVISED：TG-03；补字段职责、对象连接/状态、兼容与验收；R3 第 61 章。 |
| 66 | TG-04 Repo 恢复 | REVISED：TG-04；补字段职责、对象连接/状态、兼容与验收；R3 第 62 章。 |
| 67 | TG-05 UAV 类型边界 | REVISED：TG-05；补字段职责、对象连接/状态、兼容与验收；R3 第 63 章。 |

## Remaining Contract Boundaries

- 全部函数 ID 中，大多数仍是 SIGNATURE_ONLY，数量见本轮验证记录；23 组关键契约的扩充不等于全量函数逐项语义审计。
- 各例子明确为调用顺序示意，省略完整身份/配置/序列化；本轮没有编译或运行示例程序。
- UAV 所有命令的完整 schema、跨模块统一线程/取消保证、TG 线格式仍需对应 Spec 定稿；本文列明当前实际行为与目标要求，不补造已存在的保证。
- 原生 requester、完整生成/会话迁移、真实网络及硬件资格仍按 Spec182 等正式证据；未在本轮升级。
- 没有独立读者可用性测试。当前正文按已核对入口补充，可理解性是作者审阅结果，不是外部用户验收。

## Evidence

构建、源码身份、回归与版面结果见 [R3 evidence](../../specs/182-native-di-python-bindings/evidence/design-r3-20260908.md)；
声明仍分别由当前/冻结目标 inventory 渲染，不共享可变正文。
