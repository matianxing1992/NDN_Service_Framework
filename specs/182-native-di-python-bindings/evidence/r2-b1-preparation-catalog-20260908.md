# R2-B1 Preparation Catalog Composition

## Design and Members

基线 e7059ff3；T003-A/B/C 局部前置已通过，T006-D/T007-B 保留有效结果。
PC-1 新增 NativeCanonicalPreparationCatalog，构造输入为完整 inspected model、owned
ONNX source、recipe profile、显式 node map、publication options 与 task byte format/limit。
每条记录由 NativeCanonicalRolePreparer 校验，按完整 descriptor 锁定，重复身份拒绝；
同一 adapter 的版本/format/limit 必须一致。源码按值转入 immutable owner，调用方修改
原输入不会替换后续出版源。不自己实现 ONNX parser、网络协议或 tokenizer。

PC-2 makePreparation(user, serviceName) 自动组合 registry、catalog inspect、role producer
与真实 NativeCanonicalArtifactPublisher；所有 callback 捕获共享只读目录，销毁 factory
不使返回的 preparation 悬空。private test-only publisher factory 仅替换 Core transport，
测试仍调用同一组合逻辑与真实 publisher，生产 API 不暴露绕过 source/role 校验的端口。

PC-3 C++ inline/external 实际 source→prepareInput→inspectModel→prepareRoles→publication
→rebind/extraction，对照冻结制品摘要；覆盖 duplicate/mismatched catalog、原输入修改、
factory 销毁与取消。payload mapping 复用 NativeCatalogModelAdapter。

此入口消费 bootstrap 已认证/锁定的目录元数据；字节 hash 检查不冒充远程目录认证。
Qwen export/state mapping、native Merge 特殊边界和默认 requester 调用仍需后续接通；
不因提供此工厂就关闭 T008-A。共享选择器 Spec182Preparation、Spec182CanonicalPublisher、
Spec182NativePlanning、Spec182V3Placement、Spec182OnnxExtraction；逐成员只读官方审查后
统一兼容树 -j4 构建，不运行 integration/MiniNDN。

## Progress

PC-1/PC-2/PC-3 STATIC_PASS / TESTS_DEFERRED。已加载官方 review-agent，当前执行者
只读核对完整新类、构造/端口组合、真实 publisher 构造及 source 消费、原 registry/
adapter 语义与 C++ 回归。No findings；补充同 adapter 两个模型身份的查找覆盖。
构造失败不发布任何对象；返回 callbacks 捕获 immutable shared state，无原 factory
引用或循环所有权。身份/取消在 publisher 副作用前检查，实际 Core IO 复用现有 owner。
整批 READY_FOR_BATCH_TESTS。仅新增 class，不改既有 class layout；复用兼容构建树。

## Result and Usage

**DONE (batch only)**。`NativeCanonicalPreparationCatalog catalog(entries, assemblyControl)`
在 source/recipe 检查通过后建立冻结 registry。调用 `catalog.makePreparation(user,
serviceName)` 得到完整准备端口，`catalog.adapters()` 供 client 使用同一 registry。
records 按完整 descriptor 查找，不把仅有 modelName 或内容摘要当完整身份。
当前没有把该入口接入默认 NativeInferenceClient；T008-A 不据本批关闭。

`PATH=/usr/bin:/bin:/usr/sbin:/sbin:/home/tianxing/.local/node-v22.23.1/bin`
及既有 `NDNSF_TOKENIZER_BRIDGE_TARGET` 下执行
`python3 ./waf -o .codex-tmp/spec182-yolo-semantic-r1/build build --targets=unit-tests -j4 -v`，
exit 0，**30.369s**。实际仅两条 Compiling（新 catalog cpp、publisher test cpp）及
unit-tests 链接，见 [build.log](../../../.codex-tmp/spec182-r2-b1/build.log)。
g++ 9.4.0 -B/usr/bin、ld 2.34、system Boost 1.71 路径已核对；Core/UAV 未重编。
vmstat 首组观察到短时换出，后组两个有效采样 si/so 均为 0；未见持续换页。

同树执行 `timeout 90s .../unit-tests --run_test=Spec182Preparation,Spec182CanonicalPublisher,Spec182NativePlanning,Spec182V3Placement,Spec182OnnxExtraction --report_level=detailed --log_level=message`，
exit 0，**70/70 cases、1596/1596 assertions PASS**，见
[focused.log](../../../.codex-tmp/spec182-r2-b1/focused.log)。新组合用例 36 条断言，
inline/external 均与独立冻结制品摘要一致。`case-manifest.json` 已登记该实际用例。
`validate_design.py` exit 0/errors=[]，见 [design.json](../../../.codex-tmp/spec182-r2-b1/design.json)；
最终 diff 检查通过。未运行 integration/MiniNDN 或容器实验。

Context Mode active health 4（过期 hash），采用仓库/CodeGraph canonical source；
两份并发 native design 文档未纳入。下一步真实模型目录 bootstrap、Qwen export/state
绑定、native Merge 边界及默认 requester 调用仍按 T008/T010 继续，不能用这个
进程内传输替身的单测签发网络 publication 或无 Python 全链资格。
