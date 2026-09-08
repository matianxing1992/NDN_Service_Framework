# T008 Inspection and Role Sources

## Design and Status

PARTIAL。源码审计发现 inspectModel 用 descriptor digest 拼出不存在实际解析证据的
`/NDNSF/DI/MODEL/...`，且只从 inspection port 取 graph；原请求的完整 model descriptor
在 prepareInput 后丢失。现改由 port 返回 NativeInspectedModel 的实际 canonical source
name/digest、manifest digest 和 graph；prepared input 保留 expectedModel，逐字段绑定
adapter 和 inspection 的结果，检查返回时 deadline。source digest 与 model identity 是
不同语义，不通过相等假设替代 catalog owner 的认证。

新增 prepareRoles 从同一 native owner 的 RolePort 取得候选认证角色契约，校验 model/
graph/manifest/adapter、精确 role/rank/artifact cover、完整身份字段、graph node 范围和
最低设备内存预算。返回未选择设备的角色供 proposeRoles 使用，不拼造 recipe 或默认来源。
ensureArtifacts 的 manifest 必须与 inspected manifest 相同。

这些 native ports 仍需真实 catalog/Repo owner 的实现与 requester 接线；此处不把端口
返回值或 fixture 当作网络认证证据。T008-A 保持 PARTIAL，不宣称已完成实际 I/O。

## Validation Plan

新增来源保真/跨模型拒绝与 role manifest/rank/artifact/resource/cancel 检查；原 preparation
fixtures 改为明确的 catalog source，不再断言合成名字。新 ABI 使用独立 consumer build 树，
仅运行必要构建和相关定向测试。raw：`.codex-tmp/spec182-t008-inspection-roles-r1/`。

V3 placement 的 15 组 oracle fixture 已改为先经过 prepareRoles，再经 admission 和
proposeRoles；复用上一轮签名和角色对照数据，不另建一份业务来源。它仅证明数据流和
身份约束连接，不证明真实 catalog source 的获取。新 DTO ABI 在独立目录配置；
Context Mode project health 仍为 NO_REAL_SESSION_EVENTS，使用仓库和 CodeGraph 回退。

## Focused PASS

r1 configure.log PASS（5.518s）、build.log PASS（300.816s），全新 ABI consumer 树，
system g++ -B/usr/bin、原冻结 NAC-ABE/SVS/ONNX/ORT/Rust 闭包，unit-tests -j4；日志保留
真实编译和链接命令。vmstat.log 后续两行 si/so 为 4/0、0/0，仅短采样，不代表全程峰值。
focused.log：Spec182Preparation、Spec182V3Placement、Spec182OfferAdmission、
Spec182NativePlanning、Spec182PlanSealer，46/46 cases、403/403 assertions PASS。
覆盖实际 source name/digest 保留、完整模型错绑拒绝、角色 manifest/rank/artifact/adapter/
node/memory budget 约束、取消与缺失端口，以及 preparation→admission→placement fixture 链。
design-validation.json errors 为空，git diff --check PASS；未运行全量回归、网络集成或资格验收。

下一步补齐真实 catalog/Repo owner 的端口实现，把 V3 角色规划结果交给 sealer 并接入
NativeInferenceClient；当前 source DTO 校验不代替签名验证或实际来源获取，T008-A 保持 PARTIAL。
