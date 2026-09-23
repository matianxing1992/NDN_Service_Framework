# UAV Simulation-to-Field Revision

## Fixed Camera Baseline — 2026-09-16

按用户和导师反馈进行小幅修订：P3 使用 camera-to-body calibration；P4 明确云台是
后续可选升级、并非当前配置；新增 P5 说明固定相机的标定、低运动时段采集、图像与
拍摄时位姿/时间对应，以及同一目标匹配。固定相机是当前选择，具体集成步骤仍为计划。
姿态估计用于估计视线，不声称消除模糊/抖动，也不声称姿态本身足以确定目标位置。
新增 [MAVLink capture metadata](https://mavlink.io/en/messages/common.html#CAMERA_IMAGE_CAPTURED)
引用只支持元数据表示；硬件标定精度和时间同步仍需验证。

同步 `inspection-illustrated.tex`、两份内容相同的 PDF、可编辑 PPTX 和导出 wrapper。
两遍 pdfLaTeX 成功，最终日志无排版/字体警告。PDF/PPTX 文字预检均为 5 页且无 findings。
117/117 PDF spans 恰好分配一次，78 个非空原生文本框、11 个原生项目符号、
4 个独立图像区域、5 个恢复链接；对象均未超出画布，FILTERTEXT 背景无可提取文字。
LibreOffice 6.4.7.2 / Poppler 全 5 页人工核对通过，新增页与源 PDF 核对无明显重叠或裁切。
未在 Microsoft PowerPoint / Google Slides 客户端实测。图形线条仍为图片，文字可编辑。

PDF SHA-256: `3b282cd51f69b021d818e50d12a2b3e70a1027fb8a8842e5dc2a2d36e4d5c080`.
PPTX SHA-256: `20dd8c45922e87ad832dd110190b028ba6587d0c6e36b3c842f24ab9c1d831da`.
可重建渲染和验证清单位于 `docs/PAPER/proposal-defense/slides/build/uav-fixed-20260916-r1/`；
编译日志和修改前备份位于 `/tmp/ndnsf-uav-fixed-20260916-r1/`，这些临时产物不入 Git。
没有产品/API、实验结果或 Spec 功能验收变化。既有 pre-commit 阻塞保持未绕过，文件待提交。
下一步在实际演示客户端确认字体和排版；以下四页导出记录为历史版本。

## Editable PowerPoint Export — 2026-09-16

本轮只转换用户指定的四页 `UPDATES_UAV.pdf`，未改 PDF、TeX、研究结论或产品状态。
复用 proposal `generate_hybrid_editable_pptx.py`，新增本目录 wrapper，按
NDN Slides Review 检查文字可编辑性、断词、图文分离和全页渲染。

| Page | Editable content | Figure handling / review |
| --- | --- | --- |
| 1 | 16 textboxes：标题、请求、全部示意图标签、证据与页脚 | 场景为独立图片，标签为原生文本；示意线索边界不变 |
| 2 | 20 textboxes，3 native bullets | 流程图底图独立，文字与正文可编辑；保留 proposed/uncertainty |
| 3 | 18 textboxes，包括表格全部文字 | 修复 assigned/calibration/alignment/uncertainty 的 PDF 断词；单元格以文本框而非 native table 表示 |
| 4 | 12 textboxes，4 native bullets | 两张照片独立；不把候选硬件写为已验证设备 |

Validation：99/99 PDF text spans 恰好分配一次；FILTERTEXT 背景无可提取文字；
最终 66 个非空文本框、7 个原生项目符号、4 个独立图像区域、4 个恢复的链接。
修正后逐文本组与源 PDF 的转换清单对照一致，所有对象边界位于画布内。
LibreOffice 6.4.7.2 导出、Poppler 1400px 全四页人工检查通过；最终 r3
渲染逐页与已人工检查的 r2 PNG SHA-256 一致，无明显裁切/重叠。
未在 Microsoft PowerPoint 或 Google Slides 客户端实测。

文字预检没有超过 100 words 的页面；唯一 minor 提示为末页的 “First”，
这里指第一次台架测试，不是首创声明，故保留。几何图形仍为图片，不声称全部
线条/节点可单独编辑；产品图片内原有商标保留为图片内容。

Source PDF SHA-256: `f41a92ecaef557067ce96941e949da4d00cd12c16fd29ba0338d911c33ef6f27`.
PPTX SHA-256: `8785e0f4417ee6c8d5dfc5485acae20e2008fde091b9a1009d300a2c15304131`.
可重建的原始清单、验证 JSON、LibreOffice PDF 与渲染图位于
`docs/PAPER/proposal-defense/slides/build/uav-editable-20260916-r3/`，不入 Git。
下一步在实际演示用 PowerPoint/Google Slides 中确认字体替换；无实验重跑或 API 变化。

Checkpoint：导出与验证完成，但普通 `git commit` 被现有 `.git/hooks/pre-commit`
拒绝（exit 1）：`Commit blocked: development-assistant files or references remain
in the Git index.` 该钩子默认通过 `git grep --cached` 扫描整个 index，既有
`.specify/memory/constitution.md:16` 等文件引用触发拒绝，不是 PPTX 检查失败。
本轮未禁用钩子、未更改其环境开关、未删除既有资料；文件保留待提交。

## Current Revision: Joint Identification of an Unknown Target — 2026-09-11

用户指出更合适的任务是“只知道大概位置，不知道目标是什么，综合多个视角共同判断”。本轮采用
该方向，替换静止汽车已知类别的重复拍摄；中途考虑的路口分区观察方案未作为最终交付。
请求携带区域/问题与采集约束，不预先提供目标类别；相机负责区域搜索与互补采集，Analyzer
先判断图像是否对应同一对象，再联合推断并返回支撑图像，证据不足或冲突时允许 uncertain。
轮状特征、平面和车状轮廓均标为示意线索，不能解释为实测检测或“线索相加必然识别为汽车”。

| Page | Revision | Boundary |
| --- | --- | --- |
| 1 | 重画未知目标、遮挡区域、三个观测方向、证据传递及联合判断图 | 类别未知；所画线索为 illustrative；绿色为 Data retrieval |
| 2 | Request region/question → ACK → Selection → application association/inference → Response evidence | 多视角减少歧义是待测收益；多机并行另与单机换视点比较，三架不是固定要求 |
| 3 | 增加 region search、same-object association、时序、联合推断和不确定性缺口 | GPS 接近不等于同一对象；NDNSF 本身不提供感知算法或准确率保证 |
| 4 | 保留两种官方产品图；台架目标改为区域采集、位姿和时间检查 | 候选设备，不是已接入硬件 |

保持4页，不恢复已删除的物理闭环/评估独立页。下一步先用有真值的互补遮挡图像验证
单视角与联合判断的差异，再接实机采集；未知类别不等于相机不清楚自身任务，也不等于已具备
任意开放类别识别。未改产品/API/实验状态。

Validation：最终 pdfLaTeX 连续两遍成功，4页，无 Overfull/Underfull/Warning/缺字；Poppler
全页检查并复查改动页面，修正示意图 Data 连线穿过标签的问题。文字预检各页不超过100词；
唯一 First 提示指首个台架阶段，并非首创声明。两份 PDF 同源同步，构建和渲染保留于
`.codex-tmp/uav-joint-identification-20260911/`，驱动日志 `/tmp/uav-joint-pass*.log`。
旧路口草案的排版记录保留于 `.codex-tmp/uav-multizone-20260911/`，不作为最终案例或运行证据。
以下均为历史修订。

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
