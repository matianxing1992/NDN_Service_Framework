# Origin Proposal Coverage Restoration

## Scope and Method

按用户要求，以 `../reference-pdfs/Tianxing_Dissertation_Proposal_Origin.pdf` 为完整参照（59 个 PDF 页面，正文印刷页 1–54），补齐当前 Proposal 的重要内容。不是逐字恢复旧稿，也不以页数相同作为完成标准。双语正文采用同一章节结构。

采用 Academic Research Suite 的局部修订与主张／证据分离原则。沿用原生 LaTeX 模块和 apply_patch，不向 LaTeX 注入 Markdown block markers，不声称完成 ARS 全流程认证。主文原有段落、三个 RQ、实验数字、授权表述和 Spring 2027 计划保留；新增模块解释框架范围和工作流程，原报告作为历史记录保留。

## Coverage Matrix

以下 Origin 编号覆盖其目录的全部正文节。子节按父节成组处理；“修正吸收”表示保留问题与有用机制，不恢复过时事实或无依据评价。当前目标均在 `en/chapters/` 与 `ch/chapters/` 对称存在。

| Origin 节 | 重要内容 | 本轮处理与当前位置 |
| --- | --- | --- |
| 1.1 | 应用动机 | 补入 `application-motivation.tex`：可行性、执行者选择、设备专属语义、描述与输入边界 |
| 1.2 | NDN 基础、名称三种职责 | 补入 `ndn-background.tex`：服务／消息／对象命名、Interest/Data、信任规则、缓存、repo、Sync；不恢复旧 packet-format 图 |
| 1.3 | 四类 gap | 修正为四类相互关联需求表，不称严格四层；关联当前三个 RQ |
| 1.4 | thesis | 保留当前可检验论点；不恢复 TCP/IP 全部面向连接等概括 |
| 1.5、1.5.1 | RQ 与答案形式 | 保留三个 RQ；答案形式由实验需求、应用覆盖及新增 `evaluation-methods.tex` 明确，不恢复五个 RQ |
| 1.6 | contributions | 保留已校准的贡献／证据边界；工程接口和应用不自动变为独立原创贡献 |
| 1.7 | 框架与应用范围 | 动机和 Chapter 4 明确 UAV/DI 各种功能用于验证；不仅多角色协作 |
| 2.1 | 比较目的 | 保留当前相同需求下比较完整方案的原则 |
| 2.2、2.2.1–2.2.2 | RPC、微服务及开放问题 | 补 RPC、streaming 与对比配置职责；不声称这些系统没有发现或异步调用 |
| 2.3、2.3.1 | ROS 与应用组合 | 补机器人中间件定位；框架章节解释 ServiceContainer；不恢复无来源的 ROS 安全缺陷 |
| 2.4 | 消息队列与事件 | 补消息解耦定位；不将网络 Data 当无请求推送，不以单一传输机制证明应用更优 |
| 2.5、2.5.1–2.5.5 | NFN、NFaaS、调用、应用框架、安全 | 保留 NFN/NFaaS/RICE/NSC/DNMP 与完整授权对比；补 CFEC、MIA-NDN、SECaaS 的目标与范围 |
| 2.6 | NDN-based service systems | 并入上述相关工作，避免重复列同一系统 |
| 2.7 | patterns 与权衡 | 补控制／对象分离、stream／完整对象、placement 与 admission 的不同职责 |
| 2.8 | 未解决问题 | 需求表、当前 RQ 及依赖准入论证共同覆盖，不恢复宽泛“他人均不支持”表 |
| 3.1 | design goals | 动机需求表 + `framework-architecture.tex` 职责解释 |
| 3.2 | v0.1 baseline、总体架构 | 保留多候选选一基础；重画软件职责图，不复用过密旧图 |
| 3.3 | collaboration extension | 保留 ACK 到计划及依赖检查；补 DI 模板与运行时绑定，避免旧稿倒填 |
| 3.4 | generic API | 补当前 C++ typed 注册／调用示例；示例为节选，不冒充独立编译程序 |
| 3.5 | message model | 原四消息表及协议图保持；新增模式与数据章节说明其应用方式 |
| 3.6 | invocation workflow、策略 | 保留主交换；新增 positive/negative ACK、静默歧义、单操作选择与多角色图的区别 |
| 3.7 | security model | 保留当前三条 ABE 理由、身份/服务权限/重放区分、grant/withdraw、替代方案和撤销限制 |
| 3.8 | v0.1 evaluation | 保留当前证据章节；不恢复旧稿未匹配比较或把历史结果推广到新扩展 |
| 3.9 | limitations | 当前边界 + 新增生命周期、对象完整性及应用语义说明；不新增未测试性能数字 |
| 3.10、3.10.1 | Targeted | 补 conditional fast path；请求级 key envelope 仍需 Selection，Selection-free 接口限制按代码写 |
| 3.10.2 | Trusted Local | 补同进程受信组合；非远程可选绕过机制，不计远程授权证据 |
| 3.10.3 | Large-Data Reference | 补身份、获取、准入三步；引用元数据按具体契约，不把旧字段列表写为统一强制格式 |
| 4.1 | UAV 动机 | 通用动机 + UAV 开场保持 |
| 4.2 | 服务清单 | 补 telemetry/status、camera、video、recording、mission 契约表 |
| 4.3 | UAV architecture | 保留 ground station/drone 双角色结构，补指定设备与本地组件边界 |
| 4.4 | 操作员状态 | 补设备／操作关联、状态时效和晚到消息判断 |
| 4.5 | 视频与录制 | 补完整操作会话、直播与录制解耦、完整对象获取和验证指标 |
| 4.6 | 飞控与安全 | 补 MAVLink/PX4 及监督命令边界；授权不证明飞行安全 |
| 4.7 | 多机巡逻 | 作为次要工作负载保留任务分配／状态意义；当前地图多视角为主要协作实验，不扩大算法验收范围 |
| 4.8 | UAV evaluation | 工作流表 + `evaluation-methods.tex`：命令、帧、记录、状态、覆盖、识别 ground truth |
| 5.1 | DI motivation | 动机及已有 DI 生命周期开场 |
| 5.2 | 切分方法 | 区分 pipeline、tensor、replication，early exit 为另一个轴；不把模型导出等同通用切分 |
| 5.3 | DI architecture | 明确应用负责模型／张量，Core 提供生命周期与命名交换；单 Provider 与多 Provider 均验证 |
| 5.4 | automatic plan generation | 修正为准备模板 → ACK 运行时分配 → 依赖执行图；允许手工模板，不冒充任意模型自动规划已实现 |
| 5.5 | dependency execution | 补 tensor name/shape/type/serialization、fan-in/out、K/V 与跨步 token 反馈 |
| 5.6 | prototype status | 保留当前源代码／局部测试／完整资格区分及历史诊断边界；不提升为所有模型可运行 |
| 5.7 | DI evaluation | 补数值 oracle、cold/warm、准备与推理分项、内存及通信；不相对无法运行的模型声称加速 |
| 6.1–6.2 | 已有与计划工作 | 保留当前 evidence 与计划区分；应用功能不是所有可选 API 必须完成的新门禁 |
| 6.3 | evaluation matrix | 当前 RQ/应用覆盖表 + 新增完整测量／反例／复用方法 |
| 6.4 | timeline | 保留 Spring 2027 与完整年份；不恢复旧 November 2026 defense |
| 6.5 | risks | 保留原当前风险；补部署版本、失败分类、状态兼容和副作用边界 |
| 7 | conclusion | 保留当前研究结论目标，不把新增工程描述变为新实验成果 |

## Evidence and Wording Review

1. 当前代码核对：`ServiceProvider.hpp` 的 `addHandler<RequestT, ResponseT>` 与 `ServiceUser.hpp` 的 typed `RequestService`；`ServiceUser.cpp:6205` 后 Targeted 当前授权检查、SelectionGatedInputV1 拒绝及 request-scoped bootstrap 条件；`ServiceContainer.hpp` 的同进程 local registry 边界。示例的 `StatusQuery`、`StatusReply` 和回调函数属于解释用应用类型，不是声称仓库内存在同名 demo。
2. NDNSF-UAV 与 DI 的广义结构沿用此前 `research-revision-audit.md` 的应用源码核对。本轮补写工作流和计划评价，不把每个场景写成新通过的集成测试。
3. [gRPC 官方核心概念](https://grpc.io/docs/what-is-grpc/core-concepts/) 核对 unary/streaming。ROS 在线文档访问被拒，本文仅保留已有 ROS 文献支持的中间件定位，不加入本轮无法核对的安全实现细节。
4. [CFEC 作者机构记录](https://www.eurecom.fr/publication/8484)、[MIA-NDN 出版商全文](https://mdpi-res.com/d_attachment/sensors/sensors-23-01411/article_deploy/sensors-23-01411-v2.pdf?version=1675745740)、[SECaaS 作者项目全文](https://ice-ar.named-data.net/assets/papers/tourani2019towards.pdf) 支持新增目标概述。没有根据摘要宣称它们缺少所有授权或协作机制。CFEC 原 bib 漏掉第一作者，本轮同步修正三份 bibliography。
5. 背景沿用正文 NDN packet/trust-schema/Sync 原始引用；不复用旧图中的过时 packet 字段。K/V 明确为启用缓存时的执行方式，缓存不是自回归生成的必要条件。
6. 新增图为可编辑 TikZ，显式 single spacing；图注区分软件依赖／处理顺序与网络包方向。英文中文使用同一示例和流程。
7. 原文的新颖性比较、历史实验 provenance、完整协作/真实大模型资格仍未闭环。本轮不能称为每句已获得形式证明或完整 final-defense 验收。

## Validation

机器检查记录见 `origin-expansion-validation.json`。四入口构建、英中文镜像一致性、引文和交叉引用、渲染范围以及既有主文 byte-preservation 分别检查。句子清单现覆盖新增模块；它是可追踪导航，不是自动真值认证。

原始构建与截图：`.codex-tmp/proposal-origin-expansion-20260911/`。新增内容的人工复核与目录覆盖不等于“所有原版文字必须原样恢复”。Slides 和实验数据在本工作单元保持不变。下一步按导师反馈决定哪些恢复的架构／流程图需要用于口头报告，避免把完整论文密度直接搬入 slides。
