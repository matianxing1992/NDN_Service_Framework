# T008 Planning and Assembly Graph Identities

## Production Source Audit

追踪维护入口而非 generic helper 后，确认两种身份空间：

- `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py:513` 使用
  `YoloCanonicalArtifactBinding`。其 `adapter.py:329` 明确区分 planning graph 与
  canonical ONNX graph；`app_sdk/placement.py:4325` 用 canonical_graph_digest 认证 recipe，
  顶层 proposal/core 的 graph_digest 仍为 planning graph。
- 同一 YOLO binding 在 `adapter.py:473` 发布 source/initializer/root 后更新
  model_manifest_digest；`app_sdk/placement.py:3261` 再 describe/certify 后封存。
  `_TinyCanonicalArtifactEnsurer` 同样把稳定 artifact 身份与加密 fetch name 分开返回。
- generic `CanonicalCatalogEnsurer` 的已激活不可变根只是另一种 owner，不能用它的前提
  替代上述维护入口。现有 Core/Repo 传输与 DI 目录认证须按实际 owner 复用。

原生 validateRoles、V3 snapshot 和 core.validate 强制角色 graph 等于顶层 planning
graph，因 fixture 恰好复用同一 digest 而未暴露。这个条件会拒绝真实 YOLO 认证 recipe。

## Repair and Static Review

NativeInspectedModel 显式携带 canonicalGraphDigest，缺失时拒绝；没有从 planning digest
自动回填的生产路径。prepareRoles/ensureArtifacts 对 canonical graph 绑定角色，返回
artifact binding 保留 planning 与 canonical 两种身份；core.validate 用后者检查 assembly，
顶层 request/offer/candidate 仍绑定 planning graph。V3 placement 要求角色共享一致的
canonical graph，offer 仍独立匹配 request 的 planning graph。摘要编码不增加新 wire 字段，
复用 SDK 顶层 graph 与各 role recipe graph 的原有位置。

新增 SDK-authored distinct_graph_spaces oracle：按现有 coordinator 顺序生成 placement，
再用不同 canonical graph 认证角色，计算真实 PlacementPlanCoreV3 digest；原生完整
preparation→placement→publication→seal 验证相同结果，并拒绝将角色/工件的 canonical
graph 偷换回 planning graph。原有 fixture 显式声明两种图相同时的测试身份，不当作模型来源。
inspection fixture 另验证保留独立 canonical graph、缺失时拒绝，不允许默认为 planning graph。

## Validation Plan

DTO ABI 改动使用全新 `.codex-tmp/spec182-t008-graph-identities-r1/build`，冻结系统
工具链/依赖闭包，-j4；只做必要构建和相邻定向验证。author.log 已成功由真实 Python SDK
生成扩展 oracle；不运行全量回归、MiniNDN、SIF 或 Tiger 资格实验。

r1 configure PASS（5.184s）、全新 -j4 build PASS（288.247s）。构建后源码复核补充
inspection 的独立/缺失 canonical graph 断言，使用 r2 独立日志目录增量编译后首次运行
相关测试；无产品改动或新的 ABI 变化。

r2 build PASS（14.448s），focused exit 201：57/58 cases PASS。distinct_graph_spaces
已成功 seal 并匹配 SDK core digest，随后篡改角色 graph 的负例期待 runtime_error，
实际独立 proposal validator 抛出 invalid_argument（modified prepared assembly metadata）。
修正测试期待，产品拒绝逻辑保持；r2 日志保留，r3 独立目录重验。

## Focused PASS

r3 build.log PASS（21.806s），focused.log：Spec182V3Placement、Spec182Preparation、
Spec182OfferAdmission、Spec182NativePlanning、Spec182PlanSealer、Spec182GrantClient、
Spec182NativeInferenceClient，58/58 cases、594/594 assertions PASS。四个 SDK sealer
场景包含 CPU、不同图摘要、GPU exact-reuse 与 multi-rank，新增身份替换/缺失拒绝通过。
r2 ldd.log 无缺失依赖；r1 vmstat-start/middle/end.log 的后续行 so=0，少量 si，
只代表短采样，不是峰值或速度比较。design-validation.json errors=[]，git diff --check PASS。
Python 扩展未重建，不以旧 ABI 扩展作为此次运行证据；未做正式请求或网络资格验收。

## Remaining Production Boundary

PARTIAL。本轮只修复图身份混用。当前 NativeInspectedModel 仍要求发布前已有 NDN source
name，ensureArtifacts 仍要求发布后的 manifest 等于 inspection manifest。这些条件尚不
支持维护路径的 request-scoped publication；下一 owner 必须区分已验证本地 source、
发布后 root/fetch references，并在可信发布后重新认证角色，不能简单放开 digest 检查。
canonical source 节点索引与模型逻辑层图的对应也必须由真实 adapter/source owner 验证。
实际 catalog owner、requester Begin/Commit 和完整资格仍未完成，不新增 DONE。
Context Mode project health 非零，使用仓库与具体路径的 CodeGraph 回退；宽泛索引查询
命中了 .codex-tmp 历史副本，已排除其权威作用。

下一原生 publisher 的复用入口已核实：ServiceUser.hpp:618/620 的
prepareServiceRequest/publishEncryptedLargeData；Core ServiceUser.cpp:4980 已负责
加密、名字、分段、scope/epoch/manifest 元数据。绑定层 _ndnsf.cpp:4539 的现有调用通过
I/O context 调度；新 DI owner 可复用已暴露的 Core postToIo，不另起 Face 或密码实现。
Core result.contentDigest 是输入明文字节摘要，result.manifestDigest 是传输描述摘要；
业务 root 的 model_manifest_digest 是 root payload 的 SHA-256，三者不可按名字猜测互换。
稳定 ARTIFACT 身份与 encryptedDataName/fetch name 必须分开保存。Canonical ONNX
源身份复用 NativeOnnxRecipeAssembler.hpp 的 canonicalOnnxSourceIdentity，不重新写解析器。
