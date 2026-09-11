# UAV Simulation-to-Field Revision

## Current Revision: Remove Former Pages 4 and 5 — 2026-09-11

按用户要求删除 physical loop 与 evaluation 两页；保留原第1/2/3/6页，重新编号为1–4。
同步可编辑源及两份 PDF。双遍 pdfLaTeX 构建成功、最终日志无排版警告，Poppler 全4页渲染核对通过。
首轮因输出目录相对路径不正确而无法创建日志，修正为仓库绝对输出目录后通过；未改变内容重试。
构建产物保留在 `.codex-tmp/uav-remove-p4-p5-20260911/`，构建驱动日志在 `/tmp/uav-remove-p4-p5-pass*.log`。
被删内容可从 Git 历史恢复；无产品/API 或实验状态变化。以下为历史版本记录。

## Current Revision: Bounded Task and Measurable Value — 2026-09-11

判断：停放汽车适合作为可控的系统集成/协作验证对象，目标静止、所需视图和可见性容易独立标注；
但普通停车场检查本身并不必然需要多架无人机或空中 GPU。原稿缺少明确输出、简单方案基线以及
“感知收益”和“框架收益”的区分，容易让读者认为为了展示协作而堆叠设备。
本轮将应用收敛为远程视觉核查：在 deadline 内获取同一辆车的 rear/roof/front 视图，返回
覆盖情况、缺失视图、标注与原图名称；是否值得多机投入留给实验，不声称完成工业级损伤诊断。

| Page | Severity | Category | Problem / revision | Evidence boundary |
| --- | --- | --- | --- | --- |
| 1 | Major | Purpose / placement | 未说明检查输出；固定 GPU UAV 无任务必要性。明确远程视图任务，Analyzer 可放地面或 UAV | Proposed controlled field test；非实机成绩 |
| 2 | Major | Argument / layer | 仅列职责，未解释测试价值。说明动态可用性、依赖数据和访问权限；使用 Request/ACK/Selection/Response Data，执行单列 application | 不把相机 command acknowledgement 当 NDNSF ACK；单机+地面处理是有效基线 |
| 3 | Major | Scope | detector 被暗示能提供完整融合结果。先验证 same-car association 与所需视图覆盖 | 损伤诊断/3-D 重建需独立算法与验证 |
| 4 | Minor | Physical outcome | 几何定位/融合前置条件超过当前最小目标。聚焦同一车辆、所需视图、有效图像和失败返回 | 飞行稳定与安全拒绝仍由本机承担 |
| 5 | Major | Evaluation | 无法区分多镜头收益与框架收益。增加单机逐视图/多机比较及同机群固定分配/动态选择消融 | 消融不证明优于其他框架；匹配场景/模型/任务，记录失败与总代价 |
| 6 | Minor | Hardware status | 保留真实产品图及控制接口，标题明确为第一阶段台架候选 | 官方图片与产品页，不是实装照片 |

按 ndn-slides-review 的“任务→机制→可观察结果→有界结论”审查修改；保持6页、可编辑 TikZ 图和
产品图片，未运行硬件/产品/飞行实验，未改变 API/Design 冻结快照。NDNSF 机制沿用现有设计，
本轮只修正应用论证，不宣称这些组合已经通过实机验证。下一步应完成一台相机/云台的有效视图闭环，
然后用同场景数据验证多机相对单机是否带来值得付出代价的收益。

Validation：pdfLaTeX 最终连续两遍成功，6页，最终日志无 Overfull/Underfull/Warning/缺字。
Poppler 全页渲染人工检查；发生文字变化的第1/2/4/5页再次检查。文本预检无超100词页面；
唯一候选提示为第6页 First，指“首个台架阶段”而非首创，人工核对不属不实声明。
首轮第2页4.28pt溢出已通过缩短正文及流程框调整解决，初始与最终日志保留在
`.codex-tmp/uav-case-value-20260911/`（最终 pass6.log）。两份 PDF 同源同步。
官方 [SIYI](https://shop.siyi.biz/products/siyi-a8-mini-gimbal-camera) 与
[Gremsy](https://gremsy.com/products/pixy-u) 产品页已核对，末页来源链接保留。
本机 Spec 改号已由 `80e64053` 完成，active feature 为184，本轮未再次迁移。
以下 revision 均为历史，不代表当前页内容。

## Current Revision: Official Product Images

用户要求最后一页增加产品图：加入 SIYI 官方商店 A8 mini 图片与 Gremsy 官方 Pixy U 图片，标题从泛称 Pixy family 收敛为实物型号 Pixy U。保留“一体机 / 云台需另配相机”的区别，缩短文字，6 页总数不变。来源与格式转换见 `assets/product-image-sources.md`；未使用生成图代替产品照片。

Validation：pdfLaTeX 两遍成功，最终日志无 Overfull/Underfull/Warning；第 6 页 Poppler 渲染人工核对通过，前 5 页提取文本逐页与上一版完全一致。两份 PDF 同源同步，原始下载和构建记录保留在 `.codex-tmp/uav-product-photos/`。无产品/API 或实验状态变化。

## Current Revision: Car Scenario and Hardware (Six Slides)

按用户要求把设施统一改为停放汽车：重画车身、车窗与车轮，视角改为 rear-side / roof / front-side，Request、定位、识别对应关系和单机验收均使用同一汽车案例。采用 parked car 保留原静态目标边界，不增加移动跟踪能力声明。
末页新增 SIYI A8 mini 与 Gremsy Pixy 系列的控制接口和适用方向，并链接官方手册；设备仅为候选，不声明已购置或集成。以下 5/10 页记录均为历史。

Validation：6 页双遍 pdfLaTeX 构建通过；设备页初始溢出经字号调整解决，最终日志无 Overfull/Underfull/Warning。全部页面经 Poppler 渲染核对，汽车绘图、正文与末页介绍一致；两个 update PDF 同源同步。证据目录 `.codex-tmp/uav-car-hardware/`，未运行产品/硬件测试。下一步仍是候选云台相机的台架验证。

## Current Revision: Five Illustrated Slides

按用户后续要求将 10 页压缩为 5 页，移除独立标题页、重复说明和参考文献专页，
保留场景、NDNSF 职责、三个真实部署缺口、物理反馈闭环、四阶段落地路线。
正文迁至 `inspection-illustrated.tex`，由原入口共享；所有图形与文字均为可编辑 TikZ/TeX。
新增三架带云台相机的 UAV 观察地面设施的场景图，分别标出 GPS/IMU、相机视锥、
GPU Analyzer 和地面站；橙色为视线，绿色为应用数据流，不作为飞行航线或实飞证据。
原科学边界保持：已知 GPS 不保证目标可见，控制 ACK 不代表成功采集，多张检测结果不等于融合识别。
以下十页审阅为历史记录，不代表当前页数。当前最终构建与渲染结果见本节末尾。

Validation：5 页 pdfLaTeX 连续两遍构建，最终无 Overfull/Underfull/Warning；Poppler 全页渲染人工核对，场景底部标签重叠已修正并复查。源码和两份同内容 PDF 同步；记录与预览保存在 `.codex-tmp/uav-short-illustrated/`。未运行产品实验，未推进任何 native 或飞行资格。下一步仍为单相机/云台台架闭环。

## Scope

用户授权修订 `UPDATES_UAV.pdf`，解释模拟案例距离真实 UAV 多视角检查的差距与实现路径。
原指定 PDF 为旧四页导出版，与维护中的 `UPDATES.tex` 不一致。本次使用现有可编辑源，
增加 `UPDATES_UAV.tex` 导出入口并同步两个 update PDF；独立 `main.tex/main.pdf` 不变。
没有产品代码、API 或验收状态变更，也没有启动 native、MiniNDN 或飞行实验。

## Slide Review Ledger

| Page | Severity | Category | 原问题 / 本轮处理 | Evidence boundary |
|---|---|---|---|---|
| 1 | Major | Claim | realistic MVP 易被当作实机；改为 simulation to field | 目标案例 |
| 2 | Major | Geometry | 已知 GPS 并不消除指向、误差和目标对应；明确仍需验证 | 不宣称现场采集成功 |
| 3 | Major | Scope | 数据流程改为 proposed，明确缺少物理反馈证明 | 保留原协作图和接口边界 |
| 4 | Major | Safety / status | autopilot 不自动具备避障；端到端效果改为待验证目标 | 无实机资格 |
| 5 | Major | Missing scope | 补充位置、云台/相机、观测和真实环境差距表 | 不把现有 CPU slice 当飞行实验 |
| 6 | Major | Missing feedback | 导航、指向、采集、检查后才能完成服务；command ACK 不等于有效图像 | 拟议集成流程 |
| 7 | Major | Perception | 区分飞机定位、观测定位、跨视角识别；加入校准/时间/对应关系 | 现有检测器不自动成为融合算法 |
| 8 | Major | Execution | 台架 → SITL/HITL → 单机 → 多机，每阶段给出可观测出口 | 全部为后续路线 |
| 9 | Major | Acceptance | 预先冻结场景和指标，真值对照，失效与重试边界 | 不虚构误差或时延数值 |
| 10 | Minor | References | 官方硬件/算法工具文档给出可点击来源 | 候选工具，不声明已接入 |

## Source Checks

- CodeGraph 首先定位 `NDNSF-UAV-APP/README.md`，随后核对原文 54–61 行：CPU slice 的验证不能替代真实 MiniNDN/SITL multi-segment 部署证据。本次不将所有 UAV 组件一概标为 stub，也不声明模拟案例整体已验收。
- MAVSDK 官方 Telemetry/Camera/Gimbal 文档：候选控制接口；设备、固件及 Gimbal Protocol v2 支持须逐项核对。
- OpenCV 4.13 calibration/3-D geometry 文档：几何原语，并非完整多视角识别系统。
- PX4 Simulation/Hardware Simulation 文档：SITL 与硬件仿真范围不等于真实飞行/视觉验证。
- 原 [1]/[2] 简写文献保留为原案例上下文；本轮不据此新增实验或算法性能结论。
- 任务起点已阅读最新 failure-log 和对应 Spec182 integration 原始失败摘要；本轮不重试该产品失败，与 UAV slides 的 PDF 验证分开。

## Validation

PASS：pdfLaTeX 最终连续两遍构建；10 页，最终日志无 Overfull、Underfull、Warning 或缺字。Poppler 渲染全 10 页并人工检查 contact sheets；最后缩短的第 2 页再次单独渲染检查，第 6 页另作全尺寸核对。
原第 2 页及新增表格/算法页首轮出现纵向溢出，已缩短文字、调整表格和字号；初始日志保留在 `.codex-tmp/uav-reality-gap/build-1.log`，最终日志为 `build-6.log`。这些是文档排版问题，不是产品失败。
运行 skill 的 `audit_slide_text.py`；原第 3/4 页密度较高，保留为技术流程参考页，新增正文页约 100–135 words（含页脚和证据），未为缩页压成小字。最终两个 update PDF 使用同一构建文件，字节一致；外部链接保持可点击。
当前源码快照及 Design API 无变更；本次仅解释演示的证据边界和拟议落地路线。下一步是台架级真实相机/云台、曝光时间与位姿配对、目标可见性验证；需另行实施，不由文档完成计为硬件 PASS。
