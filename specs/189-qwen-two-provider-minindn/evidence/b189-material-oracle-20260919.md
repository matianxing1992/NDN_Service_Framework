# T007 Material Event Oracle

## Design binding

2026-09-19 01:23 -05:00。T007 item 2/3、T006 material-only consumer 与
[placement contract](../contracts/placement.md)；基线 HEAD `1d607d3e` 加既存工作区。
本单元修正 log checker 对原子材料的过期假设，不修改公开 API 或 production wire。
生产组装器已记录 root、material-manifest 和多个 material-payload；旧 checker
只接受 root/source/initializer 并按 kind 唯一计数，会拒绝合法层包链。

`examples/Spec189MaterialFetchOracle.hpp` 提取 CLI 的原有日志读取、Marker、
PlacementObservation 与材料验证逻辑，供同一 CLI 和独立 C++ parser fixture 使用。
获取状态以 `(kind, name)` 为键，每对象严格 begin→returned→verified；返回内容摘要、
大小与 verified 一致，root 与 Selection digest 一致。root verified 先于 manifest
begin，manifest verified 先于 payload begin；结束时要求一个 root、一个 manifest、
至少一个 payload，所有启动的对象完整结束。拒绝旧整 source/initializer 模式。

Stable exit：真实 CLI 使用更新后的材料 checker；C++ 正例支持多 payload，反例拒绝
授权前获取、错身份、缺对象/未完成、重复、错摘要/大小和无认证父对象。
Risk class：validation correctness；Dynamic profile：none（同步文本解析，无新线程或
生产生命周期）。C++ fixtures 验证判据本身，不计为模型或协议资格。

## Review trace

Snapshot：`.codex-tmp/spec189-material-oracle-review-r1/`，包含新增未跟踪 header/test；
`base/Spec189TwoProviderOracle.cpp` 保存本单元前的真实工作副本。
冻结四个文件，无关脏文件不混入差异；wscript 只新增 parser test target。
官方只读 reviewer `/root/spec189_review2` 已完成任务与单成员批次组合审查，均为
STATIC_PASS；确认真实 emitter 字段和状态序列与 checker 一致，无控制性缺陷。
实际加载 `/home/tianxing/.codex/skills/review-agent/SKILL.md`，SHA256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`。
审查使用完整冻结 diff、`rg` 定位 producer、逐行读取 header/fixture、SHA256 核对；
header 自由函数均 inline，field 返回值无悬空引用。迁移 lane 没有 API/schema 改动，
资格证据保持 PARTIAL，而不是因本次静态门转为 PASS。

| Lane | Scope / Status |
| --- | --- |
| production entry/callers | covered：读取 NativeCanonicalOnnxAssembler logMaterialFetch/fetchPlainObject（250–301、412–438、449–522）及 CLI 调用 |
| implementation and wire | covered：读取 shared header state/identity checks；不改生产 wire |
| test/harness/oracle | covered：逐项读取 tests/fixtures/spec189/material-fetch-oracle.cpp 正负例及拒绝判断 |
| build/source closure | covered：读取 examples/wscript 的 CLI 与新独立 parser target/include/source；无新增外部库 |
| migration/evidence | covered：拒绝旧整模型事件；真实 causal/output/reuse 仍由 T007/T009 完成 |

| Reviewed source | SHA256 |
| --- | --- |
| Spec189TwoProviderOracle.cpp | `a26964c8f0e4ea4674e7cbfbd759e9d9765560f9843c2c1db8c1837c294328b9` |
| Spec189MaterialFetchOracle.hpp | `d4f26fa2207da3f9df6f03ed0be7f40a7ff9d86e5bef5a7868c389ba7815d6b7` |
| examples/wscript | `1e4badec2b57a5a262c006b3d7633ae47a279bd1144d0a0d22c4c06db2245182` |
| material-fetch-oracle.cpp | `e0d26cd9b8391e12a6d1f8c9e1704316cc4889dd7984a2b18563bda76cd61294` |

## Validation

Context Mode active health exit 5（缺 project ContentDB），采用仓库权威回退。
Spec Kit entrypoints 同步检查 11/11 PASS。CodeGraph explore 完成但给出大量无关
validate 符号与旧临时快照；源码核对使用定向读取，不宣称图查询已覆盖。

```bash
cd build-spec189-b189-3-global-r3
env PATH=/usr/bin:/bin:/usr/sbin:/sbin /usr/bin/python3 ../waf build \
  --targets=spec189-two-provider-oracle,spec189-material-oracle-tests -j4
examples/spec189-material-oracle-tests
examples/spec189-two-provider-oracle --help
```

2026-09-19 01:25 -05:00：增量构建 PASS（23.813s，仅两个 C++ 文件编译及其链接），
parser 15 cases PASS，CLI help PASS。原始记录
`.codex-tmp/spec189-material-oracle-build-r1/{build.log,build.rc,tests.log,tests.rc}`。
构建后四个源码 SHA256 与审查快照一致。

提交前 `git diff --cached --check` 检出新 header 的 EOF 空行；仅删除一个尾部换行，
最终 header SHA256 为 `b47139d9dbbea621f8d91035472caa613828e73270772203a69bb22c72633fa2`。
逐字节检查确认 `tested_snapshot == final_header + '\n'`；已测试二进制对应上表原摘要，
此非语义清理不重建，其他源码未变。
同一 reviewer 以完整 `diff -u` 确认只删除 EOF 空行，复审 STATIC_PASS。

| Binary | SHA256 |
| --- | --- |
| spec189-material-oracle-tests | `724b2960c2350bc3763583fabf18237c8948949b197261fb9bcbbafdeeb5aa5a` |
| spec189-two-provider-oracle | `76e86f789b9bf2ed5ccd1627ea957b209e9a2de3ed6fcee2045f0281c8871e7b` |

## Closure

共享 checker 和 C++ 反例通过，本 checkpoint 只收录新 header/test、单独 target 注册
及本轮证据/进度。CLI 接线与其既存未提交修改保留工作区，待完整 oracle 整合；不把
工作区二进制冒充纯 checkpoint 构建。T007 保持 PARTIAL，其 upstream/assembly 因果顺序、cache-hit、独立输出
与多请求适配仍未完成，T009 没有真实 Qwen 两 Provider MiniNDN PASS。

Retrospective：static 提前发现旧整模型事件假设；compile-link、runtime-test 本单元无
新失败；unobserved 为完整网络材料获取、输出、同 handle 两请求及资源 drain。
