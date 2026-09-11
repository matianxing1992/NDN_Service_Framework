# UAV Simulation-to-Field Revision

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
