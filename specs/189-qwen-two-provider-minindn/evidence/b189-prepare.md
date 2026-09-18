# B189-1 Preparation Evidence

**Status**: IN_PROGRESS / NOT_NATIVE_PASS

## B189-1a worker and key ownership — 2026-09-18 15:42 -0500

官方只读 `review-agent` 已完成冻结十文件复审，结论为 `STATIC_PASS`（无 P0/P1/P2
控制性缺陷）。复审核对了 worker、取消、key owner、RAII join、rollback 和 fixture
接线；没有运行构建或测试，因此这里只关闭静态门，不关闭运行出口。
本轮 Core/DI 实现已补齐草稿并冻结十文件送审：

- `publishEncryptedLargeDataFromWorker` 复用同一 Core implementation，强制文件路径；
  hash/encrypt/range commit 在调用 worker，key/NAC/IMS 与 scheduler 投递到 Face。
  同步等待小型 I/O callback 结算，不遗弃捕获局部状态的 callback；调用方必须保持
  User/Face 存活和运行。DI prepare 的发起线程 join 发布 worker 后才释放 preparation
  ticket；其他共享 waiter 可提前取消。旧 request-time publication 保持原调度入口。
- 原 NativeRequestControl 经 transport 到 Core/store，hash/encrypt/write 的有界窗口及
  commit 前后检查；不声称可立即中断任意 OS fsync。独占事务 rollback 仍使用已审身份 fence。
- file publication 接管 wrapped-key reference，正常 TTL/lease GC、失败与 abort 都由
  RAII 释放。Core 生命周期失效 state 避免晚到 token 引用已析构 crypto。
  受保护文件发布前在 I/O 安装小型 wrapped-key data，避免并发发布借用尚不可达的 key。
  receipt 标记 fileBacked，Runtime/DI 回滚沿用；一次移除 source/initializer/root 文件，
  不再对文件 owner 已管理的 key 重复手工扣引用。旧非文件发布保留原控制路径。
- C++ fixture 增加慢 store 下 I/O heartbeat、取消/worker drain、hash/write/postcommit
  rollback；TTL 和取消后释放 fixture seed，再检查真正 wrapped-key 引用消失；
  现有真实 Core publisher case 追加 prepare worker 路径。fixture 在断言退出时先 drain，
  无法 drain 则进程失败，禁止销毁仍被 worker 使用的 Face。

**Remaining**: 真实 ModelPreparationCache/package owner 释放反例；
组合门后统一 scoped build/install/C++ selectors；设计/API 同步。尚无新增 native PASS，
未运行模型/SIF/Tiger，未提交此未验混合源码单元。
**Review trace**: 官方 `/home/tianxing/.codex/skills/review-agent/SKILL.md`，
SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`；
冻结范围为上节列出的十个文件，复审期间未改动待审范围。
**Four miss classes**: static=worker/cancel/key scope 已覆盖；compile-link/runtime-test=
本轮未运行；unobserved=cache/package 反例、组合构建以及最终两 Provider 资格。
**Closure decision**: OPEN_FOR_NEXT_BATCH，保留实际全 Spec 目标。

## B189-1a ownership repair — 2026-09-18 15:10 -0500

Base `3e53fec5`，本轮继续实现，保留其余工作区改动。

- Publisher receipt 索引改为 weak serving pins；命中时原子获取全部 pins，失效则删除，
  插入时清理过期项。package/request 仍是强 owner，publisher 不再延长材料寿命。
  新增 `Spec182CanonicalPublisher/PreparedReceiptIndexDoesNotRetainServingLeases`，
  覆盖存活复用、释放、下一 prepare 重新发布；transport seam 不等于实际 package 淘汰验收。
  官方只读 reviewer 对三文件冻结返回局部 STATIC_PASS：hpp `5f2b1af1...b84a7e`、
  cpp `6aa68d29...4019a22`、fixture `468512ab...841bf`；未构建/运行。
- Repo adapter 每次提交生成唯一 operationId；RepoCore 增加锁内身份绑定的
  `getRangeIfCurrent/removeIfCurrent/abortRangesIfOwned`，以及独占新对象的
  `putRangeIfAbsent/commitRangesIfOwned`。保留已有 name-only API 和共同实现，
  防止 has 检查后抢占、旧 lease 误读/删除替代对象或回滚他人 reservation。
  新 C++ cases：`OldLeaseCannotReadOrDeleteSameNameReplacement`（含相同 bytes）、
  `StaleTransactionCannotAbortOrOverwriteAnotherReservation`。四文件已冻结送审。

仍须完成：Core 大文件 hash/encrypt/Repo commit 移出 I/O，窗口级取消与 owner drain；
正常 serving 过期时 wrapped-key 引用释放；真实 publisher/package/cache eviction fixture；
组合静态门、匹配全局 Core/Repo/DI ABI 的统一构建和 C++ 测试、API/设计文档同步。
这些未完成项不是已知源码修复的替代项；B189-1a/T003 仍 PARTIAL，未运行完整模型。
当前约 34 GiB 空闲，未启动竞争构建或额外模型进程。

**Five lanes**: caller=prepare receipt/cache 与 ServiceUser range-store；implementation=
weak pin/RepoCore mutex 内事务身份；test=上述 C++ cases，真实 package eviction gap；
build=既有 unit-tests 与 spec189-encrypted-repo 注册，未构建；migration/evidence=
保留前置脏源码、原 API，新增 API 尚待文档/ABI交付。
**Four miss classes**: static=修复 retention/identity，worker/cancel/key release 待做；
compile-link=未运行；runtime-test=未运行；unobserved=完整 B189-1a 与真实 MiniNDN。
**Closure decision**: OPEN_FOR_NEXT_BATCH；不将局部静态通过计为功能完成。

**Review trace**: 官方 skill 路径/SHA 沿用上一条审计（`07079efd...f92228`）。
retention 三文件完整 SHA-256：hpp
`5f2b1af12632bdc188b446bab49689239341d8a2bc93ff91ca8b2f3f66b84a7e`；cpp
`6aa68d29407ae9c09f175708b0fd3d3a451334d0108e5338b13cfb9374019a22`；fixture
`468512ab5af9b199eaa5d717cb3ea7217f84842a1520d0a25640b6a6b61841bf`。
identity 四文件完整 SHA-256：RepoCore.hpp
`d66b246bec48f3182330fbf2fe71f197dd49ecffcd5459723dde33b9b88e670f`；RepoCore.cpp
`91c2b533f697893425d6b324ad4066246f3e53963e5ff5470be9cd5708fa37e5`；adapter
`873f815525f208f094c48782247d5637dd7528cac76476ac2907ffcb3c8ed7a2`；fixture
`c0d03f99451d787c33e220cad2a1e1588a47acc897b977f81246b6d4daa1d590`。
源码范围保持冻结，审查 trace 不代表编译或运行通过；未提交尚未批末验收的混合源码单元。

## Protected range-store implementation — IN_PROGRESS

Design binding: Core 新增 `EncryptedLargeDataRangeStore::commitFile(name,path,size)`
和 `EncryptedLargeDataRangeSource::{size,read}`，只处理 ciphertext。
Repo adapter 用 1 MiB window 计算摘要并执行 putRange/commitRanges/getRange；独占
新加密 name，重复 name 拒绝且不删除旧对象。返回 shared source 是读取/GC lease；
最后一个 source owner 释放后删除本次独占提交对象，失败 abort staging。
ServiceUser 保留原名字、AAD、wrapped key、签名与 segment/final-block；提交成功后
删除原 spool，不双份保留密文。新增 retainWhileLeased 参数默认 false，DI prepare
publisher 显式 true，并把 serving pin 放入 NativePreparedCanonicalPublication。
Core 的到期回调在 pin 存活时推迟回收，最后 pin 释放后在下一次检查回收。
RuntimeConfig 的 encryptedRangeStore 在两条 Core transport 创建路径注入。
requester 可配置 encrypted_repository，维护 MiniNDN 配置启用；未切换到 plain
RepoSourceProvider publisher。原子层/shared material producer/consumer 仍是下一出口。

命名/所有权边界不变；Core/DI public layout 和方法 ABI 已变化，测试前必须重建
受影响消费者并全局安装匹配库。新 C++ target `spec189-encrypted-repo` 覆盖真实
ServiceUser→Repo→Interest分段读取、签名/解密、越过 TTL、最后 lease 回收及重名拒绝。
同批复用 spec188-bounded-large-data-publisher 和 Runtime prepare selectors。
这些改动尚未通过静态门或测试；当前设计/API 文档同步仍待完成，保持 PARTIAL。
Core 文件含先前未提交的 file-backed/segmented publication 前置实现，必须将其
作为实际源码上下文审查；不将工作区其他文件加入本冻结范围或据此提交。

## T003 publication source ownership — 2026-09-18

### Next protected production binding

只读 review-agent 复核确认 plain Repo publisher 与生产 encrypted fetch 不匹配；
当前 receipt.validate 只检查 name/digest，不能提前拦截这种能力错配。T003 后续采用
[protected binding](../contracts/model-preparation.md#protected-repo-integration-binding)：
Core 保留加密、命名、签名与 wrapped-key；Repo 存加密 envelope 并提供有界 range owner；
prepared package/request lease 保住 serving 与 key，替代仅靠 5 分钟 TTL。
这仍是 TARGET，不计实现。没有选择“只接 source lookup 就完成”的较小替代方案。
下一实现范围：Core ServiceUser 存储/retention 接缝、Repo range adapter、Runtime/
requester 注入与 package lease；其后接同一接缝的原子材料 producer/consumer。

**Updated**: 2026-09-18 14:26 -0500 — 源借用修复定向验收通过；T003 整体仍 PARTIAL。
原生 DI 目标已安装到 /usr/local，build/installed SHA-256 均为
`35b54b8fb28bf5c1fdd79c8814f2fc49a630c038e1c1e74e40c242f4d52b6238`。
没有改变 Core（两侧 hash 相同），没有使用临时 loader 路径。r3 连续三轮：
`spec185-runtime --run_test=Spec185Runtime/PrepareSuccessUsesTheProductionRuntimeEntry`
每轮 1 case / 38 assertions；
`spec189-prepared-request --run_test=Spec185PreparedRequest/Spec189PreparedHandleAllocatesReferenceOnlyRequests`
每轮 1 case / 16 assertions，均 PASS。每个命令 timeout 60s；r3 日志独立保留。
这证明对应小模型准备/同 handle 无新增发布行为，未测 Qwen 峰值或完整网络链。

**Five lanes**: caller=User::prepare/prepareAsync；implementation=同步 const 引用与
ModelPreparationCache 发布后 release；test=上述生产 Runtime selectors（异步 Repo 冷发布
仅静态覆盖，未声称独立动态覆盖）；build=两个现有目标增量 -j4 / 56.712s 与 global
installed identity；migration=无公开签名、wire、异常或所有权契约变化，仅消除内部拷贝。
冻结 Runtime.cpp 与测试源码 cmp 一致。官方 skill 路径/hash 沿用 B189-4 review trace。
**Four miss classes**: static=发布源深拷贝被发现并修复；compile-link=构建通过；
runtime-test=旧 installed DI ABI 导致 heap corruption，更新全局库后三轮通过；
unobserved=real Qwen 原子材料、受保护 Repo 可达性、完整峰值、全链资格。
**Batch growth decision / Closure decision**: 本内存修复出口关闭；不扩为全局重构。
T003 继续真实 protected producer/consumer 接线，不把本次 PASS 当 T003 DONE。

原生 install 成功（8.929s），但 Waf 附带 Python editable hook 因未传
`NDNSF_GLOBAL_NATIVE_DIGESTS` 被既有门禁拒绝；未授予 binding PASS，未更改门禁。
原始安装输出 `.codex-tmp/spec189-t003-source-borrow-r3/install.log`，旧 DI 备份同目录。
本修复保持 API/ABI/既有 source 生命周期契约，无新增设计接口或当前/目标 PDF 语义变更。

r1 historical validation: STATIC_PASS (source-borrow patch SHA-256
`91ba35350dc2aab920580afa5549930a31ac3aae4186ef3d71ef005d124be829`, official read-only
review-agent)，-j4 scoped build 56.712 s 成功；Runtime prepare selector 发生
`double free or corruption (out)`，24/25 assertions 后 abort，进程 exit 139。
最后 checkpoint `di-runtime.t.cpp:497`，不代表故障源就在该行。
`.codex-tmp/spec189-t003-source-borrow-r1/` 保留 build/runtime 原始日志；
后续 prepared-request 未执行。下一步独立 debugger run 定位，未计通过或提交源码。

r2 debugger 重现 publication 析构崩溃；当前测试实际加载 /usr/local 旧 DI。
GDB `set language c++; p sizeof(ndnsf::di::NativePreparedCanonicalPublication)`：
installed=304，current build=360；Core 库哈希相同。确认同 SONAME 的旧 DI ABI 问题，
非只读借用分支（失败 fixture 实际走 Core fallback）。Changed gate：全局安装当前 DI，
验证 loaded hash 与 build 一致后重测；保留旧库备份，不以临时路径绕过。

Design binding: Runtime::User::prepare/prepareAsync 的 RepositoryArtifactPublisher
分支目前调用 sourceFor()，在同步 publish 前深拷贝整个 canonical graph/initializer。
改为 sourceRefFor() 的 const 借用；publisher 签名保持 const NativeCanonicalSource&，
ModelPreparationCache 在 publish 返回且 package 建立后才 releaseTransientSource，
publisher 不得把该引用保存到调用之外。消除同步/异步两条实际接线上的重复源 buffer，
不宣称已接通真实实验 Repo，也不变更公开签名、wire 或 READY 条件。
验证沿用生产 Runtime Repo prepare/同 handle request C++ selector，不新增镜像式测试。
本单元无新增 API；受保护 wire→Repo adapter 与拓扑无关材料仍是 T003 后续必要接线。

The real local Qwen3-0.6B snapshot was loaded and a two-stage artifact export was completed in
`.codex-tmp/spec189-qwen-two-provider-20260918/`. The current external canonical graph includes
the same dynamic `past_key.*`/`present_key.*` state family as the staged artifacts, passed
`onnx.checker`, and opened an ONNX Runtime CPU session with 59 inputs and 57 outputs. This is
preparation input only; no native `prepare`→Repo commit or reusable `PreparedModel` has been
observed yet.

| Artifact | Size | SHA-256 |
| --- | ---: | --- |
| stage-0-qwen.onnx | 752094335 | `13d8d73c0bf458b2efa2e6a91313fc8e595af81aa9f8c876be0125c040d75984` |
| stage-1-qwen.onnx | 752097486 | `585cce4d4046a07f2d73865c6d47ab194915525c14b0d4e9fd2bb3fb62708dad` |
| canonical-qwen-external.onnx (dynamic KV) | 951819 | `4b41d41cab07f69021bfc3d2c7e9fb6d71aec0554c463acb74cedc611ffbcbff` |
| canonical-initializer.bin | 1503264768 | `413814d6166b5958623e45c085c502f56498e45769c93de45b8c16c2f61c62dd` |

The export required a temporary Python 3.8 compatibility overlay and exporter corrections for Qwen3
`head_dim`, cache dtype and PyTorch 2.4 eager attention. Those changes are not production evidence.

### Canonical re-export attempt d01 — exporter harness boundary (2026-09-18)

The first dynamic-KV canonical export command stopped before model loading with
`ModuleNotFoundError: llm_pipeline_lib`; the temporary exporter omitted the
repository `.codex-tmp` module path. Raw log:
`.codex-tmp/spec189-qwen-two-provider-20260918/canonical-dynamic-export.log`.
This is an exporter harness boundary and carries no model or native runtime result.

### Canonical re-export d02 — dynamic state contract (2026-09-18)

The canonical graph was re-exported with 28 `past_key.*`/`past_value.*` inputs and
28 `present_key.*`/`present_value.*` outputs, then externalized to the candidate
paths above. `onnx.checker` and an ONNX Runtime CPU session both passed. The
semantic node mapping was regenerated for the 7,343-node canonical graph and
covers every node exactly once. Export logs are
`.codex-tmp/spec189-qwen-two-provider-20260918/canonical-dynamic-export-d02.log`
and `canonical-dynamic-externalize-d02.log`.

## Five-lane coverage

- production entry/callers: `gap` — native `Runtime::prepare` has not consumed this candidate;
- implementation/wire: `covered` for artifact graph/initializer shape only, Repo publication `gap`;
- test/harness/oracle: `covered` for ONNX checker/CPU session, C++ prepare oracle `gap`;
- build/source closure: `gap` — affected native target not yet rebuilt for Spec189;
- migration/evidence: `covered` for hashes above, run identity still `IN_PROGRESS`.

## Closure decision

`OPEN_FOR_NEXT_BATCH`: implement and run T002/T003 native prepare/Repo path. Export-only output must not be promoted.

## Prepare-time Repo boundary r1 — 2026-09-18 09:25 -0500

The production `Runtime::prepare` path now accepts a separate
`RepositoryArtifactPublisher`. When configured, it publishes through the Repo
owner during preparation and stores the immutable receipt in the prepared
package; request execution does not call the publisher. The Runtime passes the
registered `modelKey`, and the catalog exposes the publication options needed by
the Repo owner. The existing Core publisher remains the fallback when no Repo
publisher is configured.

`RepoSourceProvider` implements this boundary for canonical source,
initializer and root-manifest objects. A cold call commits the source/root
objects; a second call is a serialized, identity-checked hot lookup. The hot
path re-reads source, initializer and root ranges and recomputes their digests,
so a manifest/payload mismatch is rejected. Durable Repo receipts set
`rollbackOwned=false`; a later package or cache failure cannot delete objects
that another prepared handle may already reuse. Non-empty
`layerManifestDigests` are rejected until a layer-payload owner is connected;
they are not advertised as published layers.

The C++ selector in `tests/unit-tests/spec189-repo-publication.t.cpp` covers:

- cold commit → hot hit and stable names/digest;
- hot receipt rollback preserving durable objects;
- source payload corruption and model-key conflict rejection;
- explicit rejection of an unconnected layer reference; and
- cancellation before writes with an empty Repo catalog.

The unit target source closure explicitly includes the RepoCore, RepoClient,
RepoNode and filesystem backend definitions. Immutable static review v3 passed
(`.codex-tmp/spec189-repo-publication-review-v3/manifest.sha256`); the fixture
permission correction (private `0700` root) passed v4 review
(`.codex-tmp/spec189-repo-publication-review-v4/manifest.sha256`).

Validation from the repository root, using the canonical global dependency
install and build tree `build-spec189-b189-3-global-r3/`:

```text
../waf build --targets=unit-tests -j2       PASS (r3 2m32.904s; r4 26.037s)
unit-tests --run_test=Spec189RepoPublication --log_level=test_suite  PASS (2 cases)
unit-tests --run_test=Spec185Runtime/PrepareSuccessUsesTheProductionRuntimeEntry --log_level=test_suite  PASS
```

The first Repo selector attempt was a fixture boundary (`repo-file-root-not-private`)
and is preserved in `.codex-tmp/spec189-b189-3-unit-repo-r3.log`; the corrected
run is `.codex-tmp/spec189-b189-3-unit-repo-r4.log`. The first Runtime invocation
from inside the build directory could not find the repository-relative YOLO
oracle; it was rerun from the repository root and passed. These are focused C++
checks only. They do not prove Qwen layer publication, payload-free two-provider
Selection, Provider execution, hidden-state handoff, MiniNDN completion or
`QWEN_TWO_PROVIDER_PASS`.

## Five-lane result and closure decision

- production entry/callers: **covered** for the prepare-time Repo publisher;
- implementation/wire: **PARTIAL** — canonical source/initializer/root only;
  layer payload publication remains an explicit dependency;
- test/harness/oracle: **covered** by the two C++ selectors above;
- build/source closure: **covered** by the global `unit-tests` build and explicit
  Repo source registration;
- migration/evidence: **PARTIAL** — no real Qwen native candidate has consumed
  the layer references.

**Closure**: T003 remains **PARTIAL**. The stable boundary is now safe and
observable, but the batch cannot advance until a real layer-payload owner is
connected and its C++ cold/hot publication evidence is added.

## Prepare-time Repo boundary r2 — 2026-09-18 09:45 -0500

The v5 static review found three lifecycle defects in the outer preparation
commit path: a superseded normal job could return a package after rolling back
its receipt; cache-hit rollback called an external owner while holding the
cache mutex; and receipt construction after cache insertion could roll back a
publication already referenced by the cached package. The repair keeps the
superseded live result committed, releases the cache mutex before external
rollback, and marks the publication committed immediately after cache
accounting succeeds. The refresh result copies its package before the commit
mark so a constructor exception remains rollback-safe.

Immutable snapshot `.codex-tmp/spec189-repo-publication-review-v7/` passed the
official read-only review-agent (`STATIC_PASS`); its manifest matched all
files and no P0/P1/P2 finding remained. The review did not run build, ASan,
TSan or dynamic cancellation/exception stress selectors.

Using the canonical global dependency installation and existing build tree:

```text
../waf build --targets=unit-tests -j2  PASS (27.326s; r5)
unit-tests --run_test=Spec189RepoPublication --log_level=test_suite  PASS (2 cases; r5)
unit-tests --run_test=Spec185Runtime/PrepareSuccessUsesTheProductionRuntimeEntry --log_level=test_suite  PASS (r5)
```

Raw logs: `.codex-tmp/spec189-b189-3-unit-build-r5.log`,
`.codex-tmp/spec189-b189-3-unit-repo-r5.log` and
`.codex-tmp/spec189-b189-3-runtime-repo-r5.log`. This is focused C++
validation only. Layer-payload publication, real Qwen preparation, two-provider
Selection, Provider execution, hidden-state handoff, MiniNDN completion and
`QWEN_TWO_PROVIDER_PASS` remain unobserved.

## Five-lane result and closure decision r2

- production entry/callers: **covered** for the prepare-time Repo publisher and
  outer cache commit/rollback boundary;
- implementation/wire: **PARTIAL** — canonical source/initializer/root only;
  layer payload publication remains fail-closed;
- test/harness/oracle: **covered** by the two Repo/Runtime C++ selectors above;
- build/source closure: **covered** by the explicit Repo source closure and
  successful `unit-tests` build;
- migration/evidence: **PARTIAL** — no real Qwen native candidate has consumed
  layer references.

**Closure**: T003 remains **PARTIAL**. The corrected lifecycle boundary is
  locally verified, but the batch still lacks a connected layer-payload owner
  and full Qwen preparation evidence.

## Prepare-time Repo layer owner r3 — 2026-09-18 11:01 -0500

The layer-payload owner is now connected to the native prepare input. Each
`NativeCanonicalSource::LayerPayload` carries a stage index, contiguous layer
range, payload digest and owned bytes. `RepoSourceProvider::publish` rejects
count, range, stage, or byte-digest mismatches before staging; the cold path
commits each immutable layer object before the manifest and records
`layerReferences` with the exact Repo name, range, digest and size. The hot
path rechecks every referenced layer object and returns the same receipt.
Layer names are included in the receipt and rollback list, while transient
source ownership is still released only after the outer preparation package
has committed.

The transaction lock is owned by `RepoCore`, so separate
`RepoSourceProvider` instances sharing one Repo cannot abort each other's
range reservations. Failure cleanup removes only objects committed by the
current transaction. Legacy manifests without layers remain reusable; a C++
selector covers this compatibility path, layer corruption, count mismatch,
cancellation and cold/hot reuse.

Immutable snapshot `.codex-tmp/spec189-t003-layer-owner-review-v3.diff`
(`b9fc1efff86ccb47387e8169954b4854af368b76452fff747bc9a256749ca5dc`) passed
the official read-only review-agent (`STATIC_PASS`, no P0/P1/P2). The review
did not observe partial-commit fault injection, ASan/TSan or large-payload
pressure.

Using the canonical global dependency identity and the reconfigured
`build-spec189-b189-3-global-r3/` tree:

```text
../waf build --targets=unit-tests -j4  PASS (1m27.718s)
unit-tests --run_test=Spec189RepoPublication --log_level=test_suite  PASS (3 cases)
unit-tests --run_test=Spec185Runtime/PrepareSuccessUsesTheProductionRuntimeEntry --log_level=test_suite  PASS
```

Raw logs: `.codex-tmp/spec189-t003-layer-owner-review-v3-build.log`,
`.codex-tmp/spec189-t003-layer-owner-review-v3-selector.log` and
`.codex-tmp/spec189-t003-layer-owner-review-v3-runtime.log`. The first reused
tree target-config failure is indexed in `docs/failure-log.md` and recorded at
`.codex-tmp/spec189-t003-layer-owner-review-v1/initial-target-boundary.log`.

## Five-lane result and closure decision r3

- production entry/callers: **covered** for prepare-time Repo layer publication;
- implementation/wire: **covered** for the generic native layer-owner boundary;
- test/harness/oracle: **covered** by the three-case C++ Repo selector and Runtime regression;
- build/source closure: **covered** by the global `unit-tests` build and registered Repo sources;
- migration/evidence: **PARTIAL** — the real Qwen exporter/catalog still has not supplied
  these payloads through native `Runtime::prepare`.

**Closure**: T003 remains **PARTIAL**. The Repo layer owner is implemented and
locally verified, but T002/T003 cannot advance to complete until the pinned
Qwen candidate is consumed by the production preparation path and its real
manifest/lease receipt is observed.
