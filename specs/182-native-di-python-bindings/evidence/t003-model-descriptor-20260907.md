# T003 Complete Model Descriptor

## Source and Review

基线 ca585ab5。NativeModelDescriptor 原来只有八个简化字段；维护 Python
ModelDescriptor 还包含完整 AdapterDescriptor 和 source_revision。NativeModelRef
将 sourceRevision 放在派生类型，使复制到准备阶段的基类描述符时丢失该字段。
NativeRequestPreparation::sameModel 也遗漏 adapter ABI/schema/能力及 revision。

新增 NativeAdapterDescriptor，保留维护类型全部 17 字段，用既有 NativeCanonicalJson
生成 Python 规范 JSON 和摘要；NativeModelDescriptor 增加 adapter/sourceRevision，
严格验证格式/精度属于 adapter 能力，兼容 adapterId/version 必须匹配完整对象。
sourceRevision 从 NativeModelRef 下沉，准备/inspection 比较完整规范模型字节。
contentDigest 与 modelDigest 保持不同含义，不改现有 wire 的内容身份语义。
绑定源码暴露完整 descriptor 及继承后的 revision，T012 整体验收仍单独负责。

新测试 fixture 显式声明测试 adapter 数据，无生产 fallback 补造 schema/ABI。
独立 Python oracle 使用维护 AdapterDescriptor/ModelDescriptor 生成六组规范字节/
摘要，覆盖 Unicode、revision、schema、ABI、数组顺序及布尔值差异。准备阶段负例
核对同 content/graph 下的 revision/ABI/schema 替换被拒绝。

## Validation

r1 configure PASS；build 终态 rc=143，日志最后进度 74/173，无 compiler error，
无成功记录，现场无残留 waf/cc1plus。终止来源未确认，不推断 OOM 或源码失败。
保留 r1 后在同一新 ABI tree 独立 r2 继续。首次运行前另完成 fixture review：
adapter 明确返回所绑定的 sourceRevision，不从请求抄回；测试 helper 也保留完整 descriptor。
布局改变需新 ABI build tree，沿用已核对系统工具链/依赖，单一 -j4。
执行相关 C++ 单元和 binding 源码语法检查；不以旧 ABI extension 作为运行证据。
graph adapter identity、完整 splitter/candidate 规范摘要及实际 inspection owner
仍待完成，本记录不关闭 T003/T008/T012。

r2 新 ABI -j4 build PASS（403.580s）；r3 最终增量检查 PASS（6.591s），
81/81 cases、1067/1067 assertions PASS，含 Spec182ClientState。六组规范字节及
descriptor/model digest 与维护 Python 逐项相同；revision/ABI/schema 替换均拒绝。
binding -fsyntax-only PASS，未运行旧 ABI extension。最后仅补 Doxygen 与文档，
没有再改变可执行逻辑。design validator 与 git diff --check PASS。

Commands:
- `python3 tests/fixtures/spec182/author-model-descriptor-oracle.py`
- `PATH=/usr/bin:/bin:/usr/sbin:/sbin:/home/tianxing/.local/node-v22.23.1/bin python3 ./waf -o .codex-tmp/spec182-t003-model-descriptor-r1/build build --targets=unit-tests -j4 -v`
- `timeout 60s .codex-tmp/spec182-t003-model-descriptor-r1/build/unit-tests --run_test=Spec182CanonicalPublisher,Spec182V3Placement,Spec182Preparation,Spec182OfferAdmission,Spec182NativePlanning,Spec182PlanSealer,Spec182GrantClient,Spec182NativeInferenceClient,Spec182ClientState --report_level=detailed --log_level=message`
- `PATH=/usr/bin:/bin:/usr/sbin:/sbin /usr/bin/g++ -B/usr/bin -std=c++17 -fsyntax-only -I. -I/usr/local/include -I/usr/include/python3.8 -I/home/tianxing/.local/lib/python3.8/site-packages/pybind11/include pythonWrapper/src/ndnsf/di_bindings.cpp`

Evidence:
- [r1 preflight](../../../.codex-tmp/spec182-t003-model-descriptor-r1/preflight.log)、[configure](../../../.codex-tmp/spec182-t003-model-descriptor-r1/configure.log)、[terminated build](../../../.codex-tmp/spec182-t003-model-descriptor-r1/build.log)、[oracle](../../../.codex-tmp/spec182-t003-model-descriptor-r1/oracle.log)。
- [r2 build](../../../.codex-tmp/spec182-t003-model-descriptor-r2/build.log)。
- [r3 build](../../../.codex-tmp/spec182-t003-model-descriptor-r3/build.log)、[focused](../../../.codex-tmp/spec182-t003-model-descriptor-r3/focused.log)、[binding syntax](../../../.codex-tmp/spec182-t003-model-descriptor-r3/binding-syntax.log)、[ldd](../../../.codex-tmp/spec182-t003-model-descriptor-r3/ldd.log)、[design](../../../.codex-tmp/spec182-t003-model-descriptor-r3/design-validation.json)。

Context Mode active health 因当前 tasks.md 索引过期失败，使用当前仓库/CodeGraph
核对。未运行 integration/MiniNDN/SIF/Tiger，不代表图来源、完整候选或请求资格。
