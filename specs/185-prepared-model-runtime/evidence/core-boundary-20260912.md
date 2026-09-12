# Core Boundary Audit and Planning Revision

**Status**: PLANNED / documentation only | **Baseline**: b0b8ada1

## Findings and Disposition

1. 协议已下沉：ServiceUser的Begin/Commit/Cancel、InvocationStream传输状态及ServiceProvider scoped registration是现有Core入口；DI实际调用这些入口。无需新四消息实现。
2. 通用运行时未充分复用：NativeInferenceClient.cpp私有SerialRequestExecutor和Operation中的等待/通知混合领域状态；原C-08还继续在DI增加相同通用责任。新增C-09 CB01–CB04和B0C纠正。
3. 领域不能过度下沉：模型catalog/runner、token/KV和会话journal决定业务完成，保留DI。Core通用完成State接领域完成裁决，不把stream final改为业务成功。

源文件与定位见[C-09](../contracts/core-app-boundary.md#source-audit)。CodeGraph查询当前Core canonical路径未命中；同名include路径仅命中staging，已拒绝，改用实际磁盘源码核对。
任务开始project及active Context Mode health均ok=true；先读最新失败：Spec184默认Provider二进制缺失，属于启动preflight而非协议失败，并核对链接证据。本轮不重试该实验。

## Scope and Verification

18任务12批，全部NOT_STARTED；B0→B0C→B1，后续顺序保留。FR-013/SC-008及traceability对应C-09证明义务。
本轮检查Spec结构、任务依赖/绑定、文档diff、PDF构建身份与修改页排版；不运行native编译/运行时资格，不宣称Core提取已实现。
当前Design历史源码基线漂移仍为既有边界，本轮只更新目标路线与双PDF构建，不作全量源码一致性PASS。
源码并行变化`examples/python/NDNSF-DistributedInference/llm_pipeline/llm_pipeline_lib.py`及`docs/failure-log.md`不纳入checkpoint。

## Validation Results

- `audit_speckit_structure.py --strict` PASS：FR13/SC8/US4/tasks18、completed0、traced13；编号顺序警告来自保留原ID并把T017/T018放到实际执行位置，不重编号历史任务。
- skill同步11/11 PASS；18任务卡/18 Design binding；327本地Markdown链接无缺失；`git diff --check` PASS。
- 双PDF构建目录`.codex-tmp/design-pdf-20260912T094934223458Z`；current91页、target98页；provenance验证通过，无Overfull/Missing character/TeX错误，双PDF各12字体均嵌入；target第83页渲染检查通过。
- Context Mode authority重索引后project/active health均ok=true。本轮未运行native行为测试。
- 首次普通commit被本机hook的全index文字扫描拒绝（历史文档已有开发助手引用），未创建提交。核对hook后使用其既有`NDNSF_LOCAL_CHECKPOINT=1`本地checkpoint入口；禁止路径检查仍启用，不绕过路径规则、不push。

## Execution Handoff

按T015安装基线→T017/T018 Core复用批次执行，每任务review-agent只读静态门、批末组合审查后共享编译/测试。
实现前重新核对当前源码和184同树依赖；本轮文档检查不替代任何C++证明义务。
