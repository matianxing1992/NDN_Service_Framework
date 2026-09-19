# Failure Log and Evidence Index

## 2026-09-19 — Spec189 chunk/receipt selector boundaries

The first run after adding external-initializer chunks stopped at the test
worker lookup because the integration selector was launched without its
existing `NDNSF_SPEC182_BIN_DIR` override; the worker target existed in the
global-r3 build tree. With the override, the first receipt migration exposed
two test-contract boundaries: the small selected-payload negative used a
pre-reservation budget and reached `DI_NATIVE_ONNX_MATERIAL_INITIALIZER`
instead of the selected-payload budget gate, and the oversized inline-root
fixture first fetched a manifest because its cap was checked too late. The
fixture now uses `fetchBudget - 1`, the production cap is checked before
manifest/receipt fetch, and the inline fixture is bounded below the root JSON
scanner limit while remaining above the 4 KiB inline cap. The final r18
snapshot passed read-only review; the worker, unit, and integration targets
rebuilt with root Waf `-j4`, and the chunk round-trip plus both receipt/selected
fetch C++ selectors passed. These were build/test-boundary failures, not
MiniNDN or protocol qualification results. Durable evidence is in
`specs/189-qwen-two-provider-minindn/evidence/b189-material-publication-20260919.md`.

## 2026-09-19 — Spec189 focused material selector fixture and contract boundaries

The first run of `Spec175NativeAssembly/Spec189MaterialConsumerFetchesOneSelectedBundle`
failed at the real worker with `DI_NATIVE_ONNX_GRAPH` because the test receipt
selected only nodes 0 and 1, which did not produce the declared role output.
The immutable fixture was repaired to retain certified role-0 nodes 0–20 and
append one valid unreachable node 21 as an actual unselected material payload.
The r19 snapshot passed the official read-only review; the affected
`integration-tests` target rebuilt with root Waf `-j4`, and both the selected
bundle and selected-payload budget selectors passed with the real
`DI_NativeOnnxAssemblyWorker`. The failed run remains a test-fixture boundary,
not a production protocol result. Durable details are in
`specs/189-qwen-two-provider-minindn/evidence/b189-material-publication-20260919.md`.

The first rerun of
`Spec189RepoPublication/MaterialManifestPublishesWithOwnedTransactionsAndRejectsCorruption`
then failed only because its assertion required fewer Repo data objects than
material payloads. `RepoSourceProvider` intentionally stores one independently
addressable range-store object per payload; bundle coalescing belongs to the
protected NDN publisher. The assertion was corrected, r20 passed read-only
review, `unit-tests` rebuilt with root Waf `-j4`, and the Repo selector passed.
This was a stale test-contract boundary, not a Repo publication failure. The
same run also passed the GrantIssuer and protected publisher selectors.

## 2026-09-19 — Spec189 r38 material-publication budget boundary

The fresh root MiniNDN run `two-provider-global-r38` reached Controller,
Authority and both Providers, then the requester stopped at the first native
preparation boundary with
`PREPARATION_FAILED / DI_NATIVE_PUBLICATION_MATERIAL_LIMIT`. The old publisher
counted the full source, full initializer, material payloads, manifest and
root against the per-role assembly budget, so the real Qwen candidate could
not publish its material-backed topology. No ACK, Selection, Provider
assembly, execution, terminal response or qualification verdict was produced.
The raw run is retained under
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r38/`;
the repair and focused C++ evidence are recorded in
`specs/189-qwen-two-provider-minindn/evidence/b189-material-publication-20260919.md`.
This is a production preparation boundary, not a MiniNDN PASS or a provider
protocol failure; a new run id is required after installing the repaired DI
candidate.

## 2026-09-19 — Spec189 canonical-publisher full-suite fixture boundaries

The newly registered `spec189-canonical-publisher` target built successfully,
but its full legacy suite reported four fixture/environment failures: two
source-lifetime/cancellation assertions, one cancellation-after-source
assertion, and one encrypted-publication fixture attempting to create a file
under a root-owned temporary directory. The new material-backed budget test
and five related publisher regressions pass when selected directly. These
failures are retained as fixture boundaries and are not converted into a
product failure or PASS; the focused results and exact scope are in
`specs/189-qwen-two-provider-minindn/evidence/b189-material-publication-20260919.md`.

## 2026-09-19 — Spec189 r39 preparation-time boundary

After the material-publication budget repair and a fresh global build receipt,
run `spec189-v39-cpp-material-budget` reached Controller, Authority and both
Providers and no longer hit `DI_NATIVE_PUBLICATION_MATERIAL_LIMIT`. The real
requester instead ended with
`NATIVE_REQUEST_STAGE_FAILED code=PREPARATION_TIMEOUT boundary=preparation`
and `DI_NATIVE_PREPARATION_TIMEOUT` after spending the preparation window in
the Qwen path. The supervisor recorded `cleanup=PASS`; no ACK, Selection,
material fetch, assembly, runner, execution, terminal response or qualification
verdict exists. Raw run data is retained under
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r39/`;
the durable evidence is
`specs/189-qwen-two-provider-minindn/evidence/b189-material-publication-20260919.md`.
This is a new production preparation boundary, not a PASS; the next retry
must diagnose the preparation cost rather than only extending the timeout.

## 2026-09-19 — Spec189 B189-3 oracle build invocation boundary

The first B189-3 post-review build attempt was launched from
`build-spec189-b189-3-global-r3` with `python3 waf`; that directory exposes the
repository entry point as `../waf`, so Python exited before Waf configuration
or compilation with `can't open file 'waf'`. The raw command result is retained
in `.codex-tmp/spec189-b189-3-oracle-build-r2/build.log` and `build.rc`.
This is a build invocation boundary, not a compile, link, runtime, or protocol
result. The retry must use the canonical `../waf` entry point and preserve this
attempt.

## 2026-09-19 — Spec189 B189-2 selector-recording invocation boundary

The first post-review B189-2 selector command ran each C++ selector to
completion (`*** No errors detected`), but the recording pipeline returned
status 1 because the requested `.codex-tmp/spec189-b189-2-preselection-build-r1/`
directory had not been created, so `tee` could not open its log. No protocol or
assertion failure was inferred. The boundary is retained in
`.codex-tmp/spec189-b189-2-preselection-build-r1/selector-invocation-boundary-r1.log`;
after creating the directory, the three selectors were rerun with durable logs
and all returned status 0. This is a test-harness recording boundary, not a
T005 protocol result.

## 2026-09-19 — Spec189 F02 Waf build-tree RPATH boundary

The first `spec189-preparation-memory` selector aborted with
`free(): invalid pointer` while destroying `NativePreparedCanonicalPublication`.
The C++ target had been linked by Waf, but its RUNPATH listed `/usr/local/lib`
before the configured build tree, so the selector loaded an older same-SONAME
DI/Core library pair. The debugger record is
`.codex-tmp/spec189-f02-build-r2/gdb.log`; the interrupted clean ABI rebuilds
are retained under `.codex-tmp/spec189-f02-build-r3/` and `r4/`.

Changed gate: the repository Waf configure path now removes the installed host
libdir RPATH (including the `/usr/local` NAC-ABE branch) while preserving
target-local Waf RPATH and explicit container runtime RPATH. After reconfigure,
`readelf`/`ldd` resolved the selector to the current build-tree DI/Core hashes
and the focused C++ selector passed. This was a loader/build identity boundary,
not a native preparation behavior result; the F02 evidence remains focused and
qualification-open.

## 2026-09-18 — Spec189 focused ONNX assembly test boundaries

The first focused rerun after the r9 static gate found two Python assembly
boundaries before any MiniNDN process: the sequential identity helper omitted
the external-data base directory, and the graph-only ONNX checker resolved the
staged sidecar against the process CWD. Both were repaired, independently
reviewed in r11/r13 snapshots, and the rerun passed `22 passed, 1 skipped`.
The raw diagnostic record is
`.codex-tmp/spec189-canonical-identity-focused-20260918/failures.md`; the
successful real-Qwen identity resource record is
`specs/189-qwen-two-provider-minindn/evidence/b189-resource.md`.

## 2026-09-18 — Spec189 r27 canonical ONNX identity resource boundary

The corrected global candidate reached real MiniNDN startup with the current
Core/DI binaries and installed Python binding, but the host guard stopped the
run before the requester launched. `canonical_onnx_identity` used
`onnx.load(..., load_external_data=True)` for the 1.5-GB external initializer;
the process reached about 4.2 GB RSS, drove swap-I/O above the 256 MiB limit,
and was stopped as `RESOURCE_BOUNDARY:swapIo`. Cleanup passed and no protocol
marker, ACK, Selection, provider fetch or execution result was observed. Raw
evidence is under
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r27/`.
This is a host preflight/resource boundary, not a Qwen protocol failure or
PASS. The changed gate is the maintained ONNX identity helper: load only the
graph protobuf, materialize one external initializer at a time, hash it, and
clear `raw_data` before the next tensor; it must pass static review and a
focused identity regression before r28.

## 2026-09-18 — Spec189 r26 Python binding ABI preflight boundary

The first corrected global candidate stopped before MiniNDN because the stale
checkout `_ndnsf` extension resolved against the newly installed Core and
reported an undefined `ServiceUser::publishEncryptedLargeData` overload. No
nodes or protocol state were started. The raw run directory is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r26/`.
The binding was then rebuilt in place against the verified `/usr/local` Core/DI
closure and imported successfully; this repairs the preflight boundary but is
not a protocol result.

## 2026-09-18 — Spec189 B189-1a runtime working-directory and spool boundary

第一次运行 B189-1a selectors 时从 build 目录启动，package fixture 找不到相对路径
`tests/fixtures/spec182/*`，Repo/publisher fixture 找不到
`examples/trust-any.conf`；切换到仓库根后这三组均通过。Runtime selector 的另一个
首次边界是旧的 root-owned `/tmp/ndnsf-large-data`（755），导致
`cannot create large-data staging file: Permission denied`。为本次运行设置专用、可写
的 `NDNSF_REQUEST_LARGE_DATA_DIR` 后通过。原始日志在
`.codex-tmp/spec189-b189-1a-runs-20260918-r1/` 与
`.codex-tmp/spec189-b189-1a-runs-20260918-r2/`。这些是测试入口和宿主 spool 权限
边界，不是协议 PASS/FAIL；后续 selectors 必须从仓库根运行并使用受控临时目录。

## 2026-09-18 — Spec189 B189-1a encrypted Repo fixture header boundary

After the preparation target include/link repair, the next scoped build stopped
while compiling `tests/integration-tests/spec189-encrypted-repo-publication.t.cpp`:
`makeFilesystemRepoStore` is declared by
`ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp`, while the fixture only
included the abstract `RepoStoreBackend.hpp`. This is a test header-closure
failure before linking or execution. The raw boundary is
`.codex-tmp/spec189-b189-1a-build-20260918-r2.log`; the repair is the one added
fixture include, pending static re-review.

## 2026-09-18 — Spec189 B189-1a scoped build include boundary

The first B189-1a composition build stopped during compilation at
`tests/unit-tests/di-runtime.t.cpp:11`: the reused `spec185-preparation` target did
not expose `NDNSF-DistributedRepo/include`, so
`ndnsf-distributed-repo/FilesystemRepoStoreBackend.hpp` could not be found. This
is a Waf include/source-closure failure before linking or any test execution, not
a package-owner or native behavior result. The raw log is
`.codex-tmp/spec189-b189-1a-build-20260918.log`; the changed gate is the two
preparation target include/link lists in `tests/wscript`. A read-only re-review
confirmed that both the header include and `ndnsf-distributed-repo` link
dependency are now present; no retry has run yet.

## 2026-09-18 — Spec189 T003 Runtime prepare native heap failure

After the source-borrow increment built successfully in 56.712 s, the focused
`Spec185Runtime/PrepareSuccessUsesTheProductionRuntimeEntry` aborted with
`double free or corruption (out)`; Boost reported its last checkpoint at
`tests/unit-tests/di-runtime.t.cpp:497`, and the process ended with exit 139.
The sequential prepared-request selector did not run. Raw evidence:
`.codex-tmp/spec189-t003-source-borrow-r1/runtime.log`; active record:
`specs/189-qwen-two-provider-minindn/evidence/b189-prepare.md`.
This is an unresolved native fixture/runtime boundary, not MiniNDN evidence.
Next diagnostic is a fresh debugger run with exception/signal stacks and actual
loaded-library identities; no PASS or causal attribution until diagnosed.

Diagnostic r2 reproduced the abort in NativePreparedCanonicalPublication's
destructor. `ldd` loads `/usr/local/lib/libndnsf-distributed-inference.so`;
GDB reports sizeof(publication)=304 for that installed library versus 360 for
the current build. The installed DI hash is 76f37523..., current 35b54b8f...;
Core library hashes match. This confirms a stale same-SONAME DI ABI boundary.
Changed gate: install the just-built DI target globally and verify identical
hashes before re-running; do not insert a temporary LD_LIBRARY_PATH.

r3 closure: build/installed DI hashes match 35b54b8f... after native install;
Runtime prepare (38 assertions) and production-handle Repo request (16 assertions)
both passed three sequential repetitions. Raw logs are in
`.codex-tmp/spec189-t003-source-borrow-r3/`. The auxiliary Python editable
install hook was rejected for missing `NDNSF_GLOBAL_NATIVE_DIGESTS`; native
installation is verified independently, and binding qualification is unclaimed.

## 2026-09-18 — Spec189 T003 reused build-tree target configuration boundary

The first T003 layer-owner build stopped before compilation because the reused
Waf tree had been configured with `with_tests=False`; Waf therefore reported
`Could not find a task generator for the name 'unit-tests'`. This is a build
tree configuration boundary, not a source or runtime result. The raw boundary
record is `.codex-tmp/spec189-t003-layer-owner-review-v1/initial-target-boundary.log`.
The same tree was reconfigured with `--with-tests --with-examples` and the
canonical `/usr` Boost plus `/usr/local` NDNSF/ONNX dependency identity; the
subsequent `unit-tests` build and focused selectors passed.

## 2026-09-18 — Spec189 B189-1 Repo publication test fixture boundary

The first focused `Spec189RepoPublication` run stopped before publication because
the test created its filesystem Repo root below `/tmp`; the parent permissions made
the root group-writable and `FilesystemRepoStoreBackend` correctly rejected it as
`repo-file-root-not-private`. This was a fixture permission boundary, not a Repo
publication result. The raw output is `.codex-tmp/spec189-b189-3-unit-repo-r3.log`.
The fixture now explicitly applies owner-only `0700` permissions and fails closed if
that operation fails. The v4 immutable static review returned `STATIC_PASS`, the
incremental `unit-tests` build completed, and the corrected Repo selector plus the
production Runtime prepare regression passed from the repository root. No MiniNDN,
SIF or qualification result is inferred.

## 2026-09-18 — Spec189 B189-1 unit target source closure boundary

The first B189-1 `unit-tests` link attempt stopped after compiling because the
target did not include the RepoCore, RepoClient, RepoNode and filesystem Repo backend
definitions used by the Repo-backed C++ fixture. This was a Waf source-closure
boundary, not a runtime result. The active `tests/wscript` closure now lists those
sources explicitly; the subsequent global-dependency build and incremental rebuild
linked successfully. The source-closure change and validation are recorded in
`specs/189-qwen-two-provider-minindn/evidence/b189-prepare.md`.

## 2026-09-18 — Spec189 B189-3 host NAC-ABE ABI boundary

The first B189-3 incremental C++ build configured successfully but stopped in
`ndn-service-framework/ServiceController.cpp` because `/usr/local` supplied an
older NAC-ABE header/library pair. The compile reported missing
`KpAttributeAuthority` methods (`getPublicParametersVersion`,
`setPublicParametersVersion`, `getPublicParametersWire`, `removePolicy`,
`replacePolicy`, `rotateKeyGeneration`). No Spec189 source or runtime selector
was reached. The failed output is `build-spec189-b189-3/`; the selected target
set used `-j4`. Evidence and the exact digests are recorded in
`specs/189-qwen-two-provider-minindn/evidence/b189-build-20260918.md`.
This is a dependency ABI boundary, not a product or protocol PASS/FAIL. The
retry uses the matching explicit local NAC-ABE install prefix and a fresh
build output.

The attempted retry with that local prefix reached 38/207 compilation tasks,
then was intentionally stopped when the host policy was tightened to require
global installed dependencies. It produced no test or runtime result. The
matching NAC-ABE, ONNX full-protobuf and tokenizer bridge artifacts are now
installed under `/usr/local`; the next configure/build will use that global
closure only.

The first global-only retry then stopped at `ServiceProvider.cpp` because the
global NDN-SVS header lacked `SVSPubSub::subscribeToProducerWithCatchUp`,
although the current source/build pair contained it. No selector or oracle
linked. The matching NDN-SVS library, headers and generated configuration have
now been installed under `/usr/local`; the next retry remains a fresh
global-closure build.

That retry subsequently reached the new Spec189 marker code and stopped at
`ProtectedRuntime.cpp`: `logRuntimeEvidence` was used without its owning
`RuntimeTiming.hpp` declaration being included. The repair is limited to that
include and requires read-only static re-review before another build; no
runtime result was produced.

## 2026-09-17 — Spec188 B188-1 r8 runtime fixture boundary

r8 源码首次 canonical Repo selector 运行在 `Spec188RepoFileBackend` 先失败：夹具把普通
payload manifest 传给 `putManifest`，生产后端按契约返回
`repo-file-manifest-payload-required`，因此没有到达同代 identity-conflict 断言。该边界是
测试夹具与已收紧 production API 的不一致，不是放宽普通 manifest 写入的理由。原始运行日志
`.codex-tmp/spec188-b188-1-r8-runtime-20260917/Spec188RepoFileBackend.log` 已保留；同轮
MemoryBudget、TieredCache 和 Range selector 分别返回 0。夹具已改用合法 segmented metadata
manifest 并在断言后清理，修复后必须重新静态复审、构建和串行运行完整 selector 组。

## 2026-09-17 — Spec188 B188-1 r8 static repair review

r7 的 erase 顺序与 physical-usage recovery 缺陷已修复，并在冻结快照
`.codex-tmp/spec188-b188-1-r8-review-20260917/` 上由官方只读
`review-agent` 复审为 `STATIC_PASS`。快照基线为
`6a1aaf507fa1129d2453e1b727628c22db9c17f6`，`files.sha256` SHA-256 为
`684c8160bea4d6a0882e36b59313afbaa8e8d824c92365a9193c12adaa1a6f73`；随后组合审查也为
`STATIC_PASS`，确认可进入统一 Repo build/test。该结果仅关闭静态门；r8 源码尚未构建，
fsync/ENOSPC/ambiguous cleanup 动态注入、ASan/TSan 仍未观测，T002/T003 和 B188-1 继续
`PARTIAL`。

## 2026-09-17 — Spec188 B188-1 r7 static review boundary

官方只读 `review-agent` 对 B188-1 Repo/Core/filesystem 快照返回
`STATIC_FAIL`。冻结快照文件集合 SHA-256 为
`4f12a4cda41ad813d0f5ff2b87e342a38d3abcdb257b57f202e4e23ebd2930a8e`，`FILES`
SHA-256 为 `f28d5b3151dfd34b75413c0838871eecca7cd1ac9d1cbd34e5e45b76829cac13`。
首个控制边界是 `FilesystemRepoStoreBackend::erase` 的删除顺序：payload 先于
manifest 删除时，metadata 删除失败可能留下可见但不可读的 manifest。第二个边界是
`RepoCore::putDataPacket` 与 `handleStore` 的普通写入失败路径未刷新 physical usage，
cleanup residue 会使 capacity 视图低估。当前没有新编译或运行结果；T002/T003 和
B188-1 保持 `PARTIAL`。原始审查快照与报告仍在本地 `.codex-tmp/`，修复后必须重新冻结、
复审并在新记录中关联测试候选身份。

## 2026-09-17 — Preliminary-evidence wording PDF check

摘要措辞重建后的首次 shell 验证用未换行的整句匹配 PDF 提取文本，因换行而提前退出；
该首界是文本检查器匹配方式，不是论文构建或内容错误。保留的临时提取文本以
`/tmp/ndnsf-execution-eng.*`、`/tmp/ndnsf-execution-chn.*` 和
`/tmp/ndnsf-execution-cmp.*` 开头；改用分段、容错匹配后正文和对照检查通过。

## 2026-09-17 — Authorization wording comparison validator invocation

摘要授权措辞修正后的 comparison 验证首轮省略了 `--render` 输出目录，导致
`validate_inline_comparison.py` 仅返回参数错误；该首界是命令调用，不是正文或对照内容。
原始构建和日志保留于 `/tmp/ndnsf-challenge-comparison.GhgtXL/`。补充独立 render 目录后，
344个新版／349个旧版单元、100页 inline comparison 和页面边界检查均 PASS；持久记录见
`docs/PAPER/proposal-defense/authorization-wording-validation-20260917.json`。

## 2026-09-17 — Authorization wording rebuild working-directory invocation

统一 RQ1 评价术语后的第一次重编译命令从已进入 `proposal-defense` 的工作目录再次拼接
`docs/PAPER/proposal-defense/en`，首界是路径解析，未产生论文输出；空的临时目录为
`/tmp/ndnsf-challenge-wording-final.68mtRp/`。随后从正确的
`proposal-defense/en` 与 `proposal-defense/ch` 目录独立编译通过，并重新生成对照稿。

## 2026-09-17 — Proposal abstract rebuild boundaries

摘要边界修正后的首次独立编译使用仓库根作为工作目录，临时 `-outdir` 找不到
`uofm.cls`；输出保留在 `/tmp/ndnsf-abstract-fix-en.log`、
`/tmp/ndnsf-abstract-fix-ch.log`。改用各语言目录及独立输出目录后编译通过。
随后重建 comparison 时，旧的 side-by-side helper 仍固定旧版50页和历史 hash，
因此拒绝当前69页稿；该 helper 未被强行放宽。使用已维护的 paragraph/alignment
生成路径重新生成129页左右对照和100页 inline comparison，覆盖检查 PASS。

## 2026-09-17 — Proposal notes closeout PDF path resolution

第44页讲稿空格修正后的 PPTX 重建首轮在 pdftohtml 输入解析失败：转换器将仓库相对
`--pdf` 路径相对于 slides 目录再次展开，形成重复路径。首界是工具输入路径，
不是 PDF 内容、PPTX 布局或产品运行。原始输出保留于
`/tmp/ndnsf-minimal-closeout-20260917-rrUno4/pptx-build.log`，首次构建目录不覆盖。
使用绝对 PDF／notes 路径和独立 `retry/ndnsf-build` 目录后 PASS，成功输出为同目录
`pptx-build-retry.log`；新旧 PPTX 仅第44页备注 XML 改变，53页画面资源不变。
同轮只读 pre-commit 仍因已有 index 的 `.specify/memory/constitution.md:16` 阻塞，
未绕过或修改 index；见同目录 `checkpoint-preflight.log`。

## 2026-09-17 — Minimal-revision checkpoint remains blocked

只读 pre-commit 预检返回1，首个命中 `.specify/memory/constitution.md:16`；
输出 `/tmp/ndnsf-minimal-revision-20260917-ZZjNNc/checkpoint-preflight.log`。
文档检查通过，未修改用户 index、未绕过 hook，无新 commit 或 push。
验收见 [minimal checklist](PAPER/proposal-defense/minimal-revision-checklist-20260917.md)。

## 2026-09-17 — Minimal-revision preservation reader boundary

首轮保留检查错误地将共享 `protocol-overview.tex` 当作本轮归档中的章节文件展开，
触发 KeyError；正文构建不受影响。首次路径复现输出保留于
`/tmp/ndnsf-minimal-revision-20260917-ZZjNNc/preservation-first.log`。
限定递归范围为归档已包含的 EN/CH chapters；共享图的 input 引用在前后图块比较中
原样保留，不声称本轮重新验证共享图的历史来源。
修复后57项检查通过，持久结果见 `PAPER/proposal-defense/minimal-revision-validation-20260917.json`。

## 2026-09-17 — Minimal-revision comparison anchor collision

正文结构收敛后的 paired comparison 首次重建在 `align()` 的重复新文锚点断言失败。
“expected answers”和“hypothesis”两条规则同时指向 `RQ1 evaluates...`；首界是
配对配置，不是正文编译或实验。保留
`/tmp/ndnsf-minimal-revision-20260917-ZZjNNc/paired.log` 与首次输出目录。
移除重复的人工锚点，保留一对一覆盖断言，随后使用独立 `paired-r2/` 重建。
见 [minimal checklist](PAPER/proposal-defense/minimal-revision-checklist-20260917.md)。

最终 `paired-final/` 与 `inline-final/` 重建通过；344新／349旧单元完整覆盖，
持久结果见 `PAPER/proposal-defense/minimal-comparison-validation-20260917.json`。

## 2026-09-17 — Comparison yellow color validation

首轮 `#FFD600` 经 PDF 导出／提取成为 `#FFD500`，严格色值分类误报旧文缺失；
逐页文字和源文件比较确认仅颜色变化。首轮证据保留在
`/tmp/ndnsf-comparison-yellow-WO2cYO/build/inline-validation.json` 和 `validation.log`。
改用纯亮黄色 `#FFFF00`，保留严格检查，在独立 `build-final/` 目录重建。
最终覆盖与颜色检查 PASS，102页逐页文字不变；结果见同目录 `validation-final.log`。

## 2026-09-17 — Proposal clarity checkpoint remains blocked

只读 pre-commit 预检返回1，首个命中 `.specify/memory/constitution.md:16`。
原始输出 `/tmp/ndnsf-proposal-clarity-20260917-MTvlYK/checkpoint-preflight.log`。
本轮正文与构建检查 PASS；不修改用户 index、不绕过 hook，没有新 commit 或 push。
见 [clarity review](PAPER/proposal-defense/clarity-review-20260917.md)。

## 2026-09-17 — Proposal clarity preservation check

首轮静态保留检查因解释成本公式时新增一次内联 `$T$` 而返回 FAIL；公式自身、
实验数字、引用、图及代码未改。原始记录：
`/tmp/ndnsf-proposal-clarity-20260917-MTvlYK/clarity-validation-first.json`。
将重复符号改为“同一区间”，不放宽数学保留检查；重新构建后复验。
此为文档检查边界，不是产品测试失败。记录见
[clarity review](PAPER/proposal-defense/clarity-review-20260917.md)。

## 2026-09-17 — RQ1 concise revision checkpoint blocked

只读 pre-commit 预检仍返回1，首个命中 `.specify/memory/constitution.md:16`；
原始输出 `/tmp/ndnsf-cost-concise-20260917-n6qe0H/checkpoint-preflight.log`。
未改用户 index、未绕过 hook、无新 commit 或 push。文档精简记录见
[cost-model re-audit](PAPER/proposal-defense/cost-model-audit-20260917.md)。

## 2026-09-17 — RQ1 re-audit checkpoint remains blocked

只读 pre-commit 预检仍返回1，首个命中 `.specify/memory/constitution.md:16`。
本轮输出 `/tmp/ndnsf-cost-audit-20260917-leEAIK/checkpoint-preflight.log`。
未绕过 hook、未修改用户 index，无新 checkpoint commit 或 push；文档验收单独记录。
见 [cost-model re-audit](PAPER/proposal-defense/cost-model-audit-20260917.md)。

## 2026-09-17 — RQ1 audit notes renderer boundary

讲稿首轮错用 XeLaTeX；T1/inputenc 前言下弯引号缺字，不能交付。
首次日志 `/tmp/ndnsf-cost-audit-20260917-leEAIK/notes-build.log`，失败构建保留于
`notes-xelatex-failed/`；改回生成器原有 pdfLaTeX 流程，不修改论文论证或产品代码。
见 [re-audit](PAPER/proposal-defense/cost-model-audit-20260917.md)。


## 2026-09-17 — RQ1 cost-model checkpoint remains blocked

文档、slides和两类对照稿已修复并通过检查；全index pre-commit预检仍返回1，
首个命中 `.specify/memory/constitution.md:16`。原始输出在
`/tmp/ndnsf-cost-model-20260917-69eQBu/checkpoint-preflight.log`。
未绕过hook、未改用户index，本轮未提交。文档 `DOCUMENT_PASS` 与提交 `BLOCKED`
分开记录，见[cost-model review](PAPER/proposal-defense/cost-model-review-20260917.md)。

## 2026-09-17 — RQ1 cost-model comparison extraction boundary

成本模型的中英文论文已构建且镜像一致；旧版对照构建在新版 PDF 的字符覆盖断言失败：
`AssertionError: ('Source extraction coverage', 1)`。首界为对照工具的 PDF 提取，
不是论文编译或产品实验。原始证据：
`/tmp/ndnsf-cost-model-20260917-69eQBu/paired-build.log`。
须定位遗漏字符及版面边界后修复，不得删除覆盖断言。文档单元暂为 `PARTIAL`，
旧版对照仍保留前轮产物；产品状态不变。同轮第20页表格间距已作视觉修订，
旧截图与PPTX保留于该run的 `lo-r1/`、`render-r1/` 和 `*-r1.pptx`。

定位为公式三个求和符号的字体fallback不一致：`get_text('dict')`默认保留CID，
`get_textbox()`默认产生替换字符。真实源PDF回归先失败（`math-regression-red.log`），
修复统一提取flags，保留原PDF矢量公式及原字符覆盖断言；随后运行完整对照回归。

关闭：20项测试通过，paired重建为134页，inline重建为103页且内容／边界检查通过。
原失败产物仍保留；这只关闭文档工具缺陷，不产生产品或实验资格结论。

## 2026-09-17 — Contribution revision checkpoint remains blocked

最终文档验证及布局修复已通过，仍因既有全index pre-commit规则无法建立checkpoint。
首个命中 `.specify/memory/constitution.md:16`；原始输出保存在
`/tmp/ndnsf-contribution-revision-20260917-oPAr8a/checkpoint-preflight.log`。
未绕过hook、未改用户index，HEAD仍 `d5b241e6`。文档 `DOCUMENT_PASS` 与
提交 `BLOCKED` 分开记录；详见[revision](PAPER/proposal-defense/contribution-revision-20260917.md)。

## 2026-09-17 — Contribution slide wording/layout regression

第二轮将第34页收束语改为proposal口径后，最终验证器发现
`Overfull vbox (0.90697pt too high)`；不是文字缺失或协议结果。
该轮产物已复制到交付位置，因此必须修复并重新覆盖验证，不能保留 `PASS` 声明。
失败JSON和TeX日志分别保留于本轮run的 `validation-r2-layout-failed.json`、
`slides-r2-layout-failed.log`。缩短该页收束语后重建PDF、notes、PPTX及回转验证。

## 2026-09-17 — Proposal contribution comparison dependency boundary

三项贡献修订的 inline 对照构建在上游 paired 构建尚未写出
`main_comparison-report.json` 时启动，返回 `FileNotFoundError`。首界是文档流水线
依赖顺序，不是论文或协议失败。原始失败日志保留于
`/tmp/ndnsf-contribution-revision-20260917-oPAr8a/inline-build.log`。
上游随后完成：131页，349个旧单元和334个新单元；重试须确认报告存在并使用新目录。
本轮 slides 初稿另发现 2.74728pt vbox 超出，收紧第7页文字/间距后最终编译无
Overfull；第一次编译日志已被同路径最终构建覆盖，不声称保留了该原始日志。
详见[contribution revision](PAPER/proposal-defense/contribution-revision-20260917.md)。

## 2026-09-17 — Proposal criteria checkpoint preflight blocked

文档构建、对照和PPTX核验通过后，只读执行当前 pre-commit 检查返回1，
首个命中仍为 `.specify/memory/constitution.md:16` 的既有全index引用。
原始输出：`/tmp/ndnsf-proposal-criteria-20260917-cGu4YP/checkpoint-preflight.log`。
没有绕过hook、没有改动用户index或尝试混合提交；HEAD仍为 `d5b241e6`。
文档交付有效，checkpoint仍 `BLOCKED`；本轮修改留在工作树，详见
[criteria review](PAPER/proposal-defense/proposal-criteria-review-20260917.md#checkpoint-boundary)。

## 2026-09-17 — Proposal criteria validator frame-count boundary

首轮验证器把有标题的 frame 字典长度当总页数，漏计无标题 titlepage，
报告 49/49，尽管 PDF 与 PPTX 均为 50 页。首界是验证脚本计数，不是 slides 缺页。
保留 `/tmp/ndnsf-proposal-criteria-20260917-cGu4YP/criteria-validation-first-failed.json`；
修正为独立计数 `begin{frame}`，结果页正文对照仍按标题逐项进行。
文档证据归档见 [criteria review](PAPER/proposal-defense/proposal-criteria-review-20260917.md)。

## 2026-09-17 — Proposal criteria build layout/configuration boundary

首轮论文 latexmk 继承 auxdir 配置，与显式 outdir 不同，故未把该轮当最终构建；
改为显式设置一致的 auxdir/outdir，保留首轮日志。slides 首轮新评价表末尾
出现 0.1362pt Overfull vbox，缩短总结并减少 1mm 间距后重新渲染。
原始日志：`/tmp/ndnsf-proposal-criteria-20260917-cGu4YP/*-driver.log`。

## 2026-09-17 — Proposal criteria patch anchor mismatch

双语补丁中的中文表题与实际源文不符，apply_patch 在写入前拒绝整批；
后续评价补丁错误假定中文也有英文的 samepage 环境，亦在写入前拒绝；
改为分别读取并匹配各语言锚点，不假设布局标记相同。
复查英文新短语不存在、中文原文仍在，确认未部分应用。首界为文本锚点，
不是内容或编译失败。改用实际表题并分开应用双语补丁；原稿备份位于
`/tmp/ndnsf-proposal-criteria-20260917-cGu4YP/before.tar`。

## 2026-09-16 — Tiger proposal checkpoint blocked

四入口论文、50页slides/PPTX/讲稿/LibreOffice回转及19项对照工具测试通过；
95页括号版和123页左右版保留320新/349旧单元。本轮48路径已暂存，119个
已授权proposal待提交路径的普通checkpoint仍被全index引用hook拒绝，首个命中
`.specify/memory/constitution.md:16`。未绕过，无新commit或push；HEAD为`d5b241e6`。
日志：`/tmp/ndnsf-proposal-tiger-plan-20260916-vvOwy9/checkpoint.log`；
证据：[Tiger proposal plan](PAPER/proposal-defense/tiger-evaluation-plan-20260916.md)。

## 2026-09-16 — Tiger proposal report anchor mismatch

更新报告时补丁中的临时路径误带空格，apply_patch拒绝整个修改，未改变文件；
首界仅为文本锚点。按原文状态行缩小匹配后重试，不重跑已通过的编译或实验。
证据：[Tiger proposal plan](PAPER/proposal-defense/tiger-evaluation-plan-20260916.md)。

## 2026-09-16 — Skill-guided proposal checkpoint blocked

四入口论文、50页slides/PPTX/讲稿/LibreOffice回转、19项工具测试及对照完整性通过。
本轮50路径已暂存；包含前轮授权proposal待提交内容的117路径普通checkpoint仍被既有
全index引用hook拒绝，首个命中`.specify/memory/constitution.md:16`。
未绕过、无新commit或push，HEAD为`d5b241e6`。原始日志：
`/tmp/ndnsf-proposal-skill-review-20260916-mDGrXA/checkpoint.log`；
证据：[skill review](PAPER/proposal-defense/proposal-skill-review-20260916.md)。

## 2026-09-16 — Proposal skill-review report patch anchor mismatch

报告更新补丁的临时目录行误带前导空格，apply_patch拒绝匹配，文件未改变；
首界是补丁锚点，不是编译或文档内容检查失败。保留本条错误记录，下一次
只匹配实际存在的状态行。证据：[skill review](PAPER/proposal-defense/proposal-skill-review-20260916.md)。

## 2026-09-16 — Proposal-language documentation checkpoint blocked

四入口论文、50页slides／PPTX／讲稿／LibreOffice回转、19项工具测试及对照完整性通过。
59个明确路径已暂存；普通checkpoint仍被既有全index开发助手引用hook拒绝，
首个命中`.specify/memory/constitution.md:16`，无新commit／push，未绕过；HEAD为`d5b241e6`。
日志：`/tmp/ndnsf-proposal-language-20260916-N4R7eE/checkpoint.log`；
证据：[language review](PAPER/proposal-defense/language-review-20260916.md)。产品状态不变。

## 2026-09-16 — Language-review comparison fixed-coordinate clipping

段落对照首轮在N0109／N0111检测到缺字并拒绝通过，括号生成随之拒绝非PASS输入。
首界是两面板提取使用固定纵坐标；正文分页调整后图和表下移，正式PDF无缺字。
保留`/tmp/ndnsf-proposal-language-20260916-N4R7eE/paired.log`、`paired/`与`inline.log`；
下一步按图注与绘图边界定位并回归纵向位移，不放宽完整性检查。
证据：[language review](PAPER/proposal-defense/language-review-20260916.md)。

## 2026-09-16 — Language-review notes alias copy path

讲稿同步在slides工作目录内使用仓库相对路径，`cp`报cannot stat，35min源未更新；
TeX编译成功，首界仅为复制路径。改用明确的slides内路径后核对源与PDF一致性。
记录：[language review](PAPER/proposal-defense/language-review-20260916.md)；
run：`/tmp/ndnsf-proposal-language-20260916-N4R7eE/`。不改变产品状态。

## 2026-09-16 — Proposal-register documentation checkpoint blocked

四入口论文、50页slides／讲稿／可编辑PPTX／LibreOffice回转及19项对照工具测试通过。
57个明确路径已暂存，普通checkpoint被既有全index开发助手引用hook拒绝，首个命中
`.specify/memory/constitution.md:16`。无新commit或push，未绕过hook；HEAD仍为`d5b241e6`。
原始日志：`/tmp/ndnsf-proposal-register-20260916-532JHS/checkpoint.log`；
证据：[proposal register review](PAPER/proposal-defense/proposal-register-review-20260916.md)。
不改变产品任务状态；解决无关暂存项／hook策略需要独立授权，不扩大本轮修改范围。


## 2026-09-16 — Latest-design proposal checkpoint blocked

四入口论文、50页slides、讲稿／可编辑PPTX／LibreOffice回转、19项对照工具测试通过；
53个明确文档路径已暂存。普通checkpoint被既有全index开发助手引用hook拒绝，
首个命中`.specify/memory/constitution.md:16`；没有绕过或新commit，HEAD仍为`d5b241e6`。
原始输出：`/tmp/ndnsf-design-sync-20260916-oD8E9i/checkpoint.log`。
证据：[design sync](PAPER/proposal-defense/design-sync-20260916.md)。文档状态DOCUMENT_PASS，
Spec187产品状态不变；并行源码及全index清理均不在本轮授权范围。


## 2026-09-16 — Design-sync read-only checker backup-path error

首轮只读检查误将已存在的备份目录写成`/tmp/ndnsf-design-sync-20260916-oD8E9i/raw/docs/`，
实际为同一run下的`docs/`，触发FileNotFoundError；未影响源码、编译或产品状态。
通过目录检查确认后改正检查器输入。详见[design sync](PAPER/proposal-defense/design-sync-20260916.md)。


## 2026-09-16 — Proposal paper/slides synchronization checkpoint blocked

四入口论文、50页slides、讲稿、可编辑PPTX及LibreOffice回转检查通过；
19项对照工具测试通过，94页括号版和117页左右版更新完成。48个明确路径已暂存，
普通checkpoint被既有全index开发助手引用hook拒绝，首个命中
`.specify/memory/constitution.md:16`；未绕过、无新commit或push。
原始输出`/tmp/ndnsf-paper-slides-sync-20260916-AsSp5o/checkpoint-r2.log`；
详见[audit sync](PAPER/proposal-defense/audit-sync-20260916.md)。
文档状态DOCUMENT_PASS；不改变产品或研究资格状态。


## 2026-09-16 — Proposal audit-sync new report paths ignored

已通过文档检查后的首次显式暂存遇到`docs/PAPER`忽略规则，新建报告和检查器
未纳入index；随后的普通checkpoint因新路径未登记失败，记录于
`/tmp/ndnsf-paper-slides-sync-20260916-AsSp5o/checkpoint.log`。
仅对本轮三个明确的新文档／检查文件使用`git add -f`后重试，不扩大提交范围。


## 2026-09-16 — Proposal synchronization validator frame-count assumption

新增检查脚本首轮只统计带标题参数的frame，漏掉titlepage，错误期待50个匹配；
产物仍为50页。首界为检查器解析假设，不是排版失败。保留
`/tmp/ndnsf-paper-slides-sync-20260916-AsSp5o/sync-validation.log`；
修复为含可选参数及无标题frame的解析后另存重试日志。


## 2026-09-16 — Proposal slide export notes-path failure

本轮PDF转PPTX首轮在讲稿注入前失败：相对`--notes-tex`被按slides目录再次解析，
导致`FileNotFoundError`，不是论文、协议或实验失败。原始输出保留在
`/tmp/ndnsf-paper-slides-sync-20260916-AsSp5o/export.log`；原始正式PPTX未覆盖。
下一次使用讲稿绝对路径和新的`ndnsf-build-r2`工作目录。


## 2026-09-16 — Proposal necessity-review checkpoint blocked

95项修改必要性分类、五处双语局部修订、四入口构建／镜像文字／修改页视觉、
11+8工具测试及349旧／307新单元完整性检查通过。39路径普通checkpoint仍被
既有全index开发助手引用hook拒绝，首个命中`.specify/memory/constitution.md:16`。
原始输出：`/tmp/ndnsf-necessity-review-20260916-77CAZE/checkpoint.log`。
未绕过hook、无新commit；交付保存并暂存，文档状态DOCUMENT_PASS，不是产品实验。
记录收尾首次patch因本文件并行新增条目导致旧上下文不匹配，在写入前被拒；
重读后只追加自身条目。记录：[necessity review](PAPER/proposal-defense/necessity-review-20260916.md)。

## 2026-09-16 — Qwen3-0.6B local preparation timeout

The current-source Qwen3-0.6B replay `qwen06b-local-real-r9` reached Controller,
Authority and all three Provider readiness, then stopped before request execution with
`PREPARATION_TIMEOUT` / `DI_NATIVE_ONNX_ASSEMBLY_TIMEOUT`. No Selection, Provider
execution, terminal response or numerical oracle was observed; this remains an
unqualified preparation failure. Raw root-owned output is retained at
`results/spec184-qwen06b-local/qwen06b-local-real-r9/`; durable context is recorded in
[`qwen06b-local-real-replay-20260916.md`](../specs/184-native-di-closure/evidence/qwen06b-local-real-replay-20260916.md).
The 1.5 GB external initializer and its canonical copy remain preserved; no retry is
started while the local YOLO path is the active validation target.

## 2026-09-16 — Qwen native requester rebuild missing ONNX C++ prefix

为验证当前 requester/Conversation 修订，本机复用 `build-spec187-local-nac-r1` 并以
系统 `/usr/bin/g++ -B/usr/bin -j4` 只选择 `DI_NativeRequester`。Waf 在
`NativeOnnxRecipeAssembler.cpp` 编译阶段首先报
`fatal error: onnx/checker.h: No such file or directory`（`rc=1`）。配置指向
`.codex-tmp/spec182-t001-dependencies/onnx-install`，该路径缺少 ONNX 1.17
full-protobuf headers、`libonnx.a` 和 `libonnx_proto.a`；找到的 Python wheel 头文件
不能替代 C++ 静态库闭包。原始输出保留在
`.codex-tmp/spec187-qwen-budget-build-r1-20260916.log`，详细上下文见
[`Qwen native rebuild evidence`](../specs/184-native-di-closure/evidence/qwen06b-local-real-replay-20260916.md)。
这是构建依赖首界，不是 Qwen 协议或 MiniNDN 结果；没有下载依赖、复制 1.5 GiB
initializer 或重跑全量运行，T007 仍 `PARTIAL`。

## 2026-09-16 — Requirement traceability Chinese label mismatch

R1/R2局部修订的英文构建完成；中文构建退出零但最终日志仍有
`chap:applications`未定义，现有中文标签实际为`chap:applications-ch`。
首个边界是交叉引用，不是产品或协议失败；未交付该PDF。原始输出保留在
`/tmp/ndnsf-comment-fixes-20260916-qQkKkU/ch-root/`与`ch-root.log`，修正后
用新`ch-root-r2/`重建。记录：
[targeted revision](PAPER/proposal-defense/comment-fixes-20260916.md)。
四入口最终构建、镜像文字及修改页视觉检查通过。随后对照生成因旧源哈希门禁
拒绝；保留同目录`paired.log`，未覆盖对照。核对已授权的四文件变更与主题后
再更新预期哈希，不绕过或删除门禁。
对照r2在完整标题门禁拒绝：旧固定页号24的图表裁剪误覆盖移页后的Participants
标题；`paired-r2.log`及`test-paired.log`保留。改为标题内容识别并增加回归检查，
以新目录重跑；不取消标题完整性检查。
最终四入口构建／镜像文字／全文边界及修改页视觉检查通过；段落工具11/11、
括号工具8/8通过，117页左右版与94页括号版完整保留349旧／304新单元。
上述文档失败已解决；未运行产品实验，不改变当前Spec资格状态。
首次checkpoint因旧资源从先前暂存新增项移除后已不是Git已知路径而拒绝；
`checkpoint.log`保留，新资源和交付暂存未丢失，旧资源有备份。重试仅传当前
存在的明确交付路径，不扩大范围。
第二次准备脚本在Git调用前因系统Python不支持`str.removeprefix`退出，保留
`checkpoint-r2.log`；改为前缀切片后重试，不改变交付范围。
第三次42路径普通checkpoint被既有全index引用hook拒绝，首个命中
`.specify/memory/constitution.md:16`；`checkpoint-r3.log`保留。无新commit，
未绕过hook；交付保存并暂存，文档检查仍为DOCUMENT_PASS。

## 2026-09-16 — Advisor-comment audit extraction boundary

只读检查旧新对照JSON时误将`units`列表当字典调用`.items()`，在读取内容前失败。
未修改论文、未运行实验；已先记录
`/tmp/ndnsf-advisor-audit-20260916-KXNkID/extraction-boundary.md`，随后按实际列表
结构复核。最终19批注及95项修改理由覆盖检查通过，P18/P19仍需文字补强；
这不是协议失败，也不将评论审查计为产品验收。
持久记录：[comment-change audit](PAPER/proposal-defense/comment-change-audit-20260916.md)。
两个新增审计文件的限定路径checkpoint再次被既有全index hook拒绝，首个命中
`.specify/memory/constitution.md:16`；同目录`checkpoint.log`保留。文件已保存并
暂存，无新commit、未绕过；不改变审计结论或产品验收状态。

## 2026-09-16 — Inline comparison validation counter

括号金色旧文对照的首次三遍XeLaTeX构建成功，但验证器使用`Counter(dict)`
而不是`Counter(dict.keys())`，导致旧文单元计数误报。已修复验证器，未覆盖
正式对照，保留`/tmp/ndnsf-inline-comparison-20260916-MtEJNq/validation-r1.log`。
修复后`validation-r2.log`通过，全文颜色通道/顺序/边界无遗漏；93页渲染与
7页放大检查完成，交付更新为单栏金色括号旧文格式，正式论文不变。
限定34路径普通checkpoint被既有全index引用扫描拒绝，首个命中
`.specify/memory/constitution.md:16`；同目录`checkpoint.log`保留。无新commit，
未绕过hook；交付已保存并暂存，文档检查结果保持通过。
记录：[inline comparison](PAPER/proposal-defense/inline-comparison-review-20260916.md)。

## 2026-09-16 — Semantic comparison mirror bibliography mismatch

四个 LaTeX 入口均编译成功，但逐页文字检查发现根目录与 en 镜像的第61页
NSC书目不同。首个边界是独立的 `en/ref.bib`、`ch/ref.bib` 未同步，
不是正文或实验失败；不能以编译成功替代镜像一致性检查。
原日志保留于 `/tmp/ndnsf-semantic-audit-20260916-KmL5Nl/validation-r1.log`。
同步两个书目副本后重建镜像，使用全新 `render-r2/` 验证。
镜像逐页文字一致性通过；最终 `render-final/` 保存229页渲染与20页放大图，
正文/对照提取和边界检查通过。未发生产品或实验失败。
限定28路径普通checkpoint再次被既有全index引用扫描拒绝，首个命中
`.specify/memory/constitution.md:16`；同目录 `checkpoint.log` 保留输出。
无新commit，未绕过hook；交付文件保存并暂存，文档验收结果不变。
持久记录：[semantic comparison review](PAPER/proposal-defense/semantic-comparison-review-20260916.md)。

## 2026-09-16 — Paragraph comparison layout API boundary

逐段PDF工具r1在文本框测量时因PyMuPDF `Story.place()`返回tuple而非Rect退出，
未生成PDF、未修改论文；显式Rect转换后使用全新r2目录。
记录：[paragraph comparison](PAPER/proposal-defense/paragraph-comparison-review-20260916.md)。
原始目录：`/tmp/ndnsf-paragraph-comparison-20260916-mV1e0G/r1/`。
r2为整页截图高度超限，未写正式PDF；按可用高度等比例放置，保留r2.log并使用r3。
r3逐框文字门禁发现PDF Form隐藏文字外溢（非可见正文重叠），正式文件未覆盖；
清除图形裁剪框外的隐藏文字后进入r4，原报告与PDF完整保留。
r4文字门禁通过但结构审查发现零高度表格线被Rect并集忽略；改用坐标min/max，
重新检查整表图形范围，不将文字PASS当作结构/视觉PASS。
最终114页对照、全644单元覆盖、逐框文字/字符顺序、7项工具测试和全页渲染检查通过。
限定6路径的普通checkpoint仍被全index钩子拒绝，首个命中`.specify/memory/constitution.md:16`；
原目录`checkpoint.log`保留完整输出，未生成commit、未绕过；正式对照已更新、文件暂存。

## 2026-09-16 — Proposal slide synchronization tooling

P4添加NDN全称后出现2.21pt、随后1.31pt vbox溢出，压缩重复结语后消除；
讲稿长行0.36pt溢出以两入口raggedright修复。首次PPTX导出因临时构建目录basename
不是`ndnsf-*`在写入前被所有权检查拒绝，不放宽检查，改用全新专用目录。
原日志/快照：`/tmp/ndnsf-slide-sync-20260916-XDVHiC/`；持久记录：
[slide sync](PAPER/proposal-defense/slides/research-structure-sync-20260916.md)。
这是文档工具与排版边界，没有运行或判断产品实验；待最终回渲检查。
首轮LibreOffice回渲发现P4/P8表格横线与上一段文字过近，机器文字一致性不能代替
视觉检查。保留`lo/`和`lo-render/`首次结果，修正TeX段落/间距后使用新目录重新导出。
同类间距覆盖P29/P37–39/P45；P29额外5.52pt溢出通过精简引导句解决。
最终四份LaTeX日志无溢出/未定义引用/缺字，PPTX为1021/1021文本片段和50页notes；
LibreOffice全部页面检查通过。`render-release`/`lo-release-render`及精简validation为最终证据。
限定11个slides路径的普通`git commit --only`仍被全index钩子拒绝，首个命中
`.specify/memory/constitution.md:16`；`checkpoint.log`保留于同一临时目录。无新commit，未绕过。

## 2026-09-16 — Proposal change review retrieval/edit boundaries

本轮首次Context Mode guard query缺`--require`，补精确项目标识后通过；压缩后timeline
查询因无显式session-event source/category被hook阻止，未用于状态判断。另一次多文件
patch因hunk顺序在写入前拒绝，按文件顺序重组后成功。以上均非产品/协议失败。
以完整Origin、修订前快照及当前LaTeX/PDF为依据，四入口编译和文档检查最终PASS。
持久记录：[change-value review](PAPER/proposal-defense/change-value-review-20260916.md)；
原始构建/快照/渲染目录：`/tmp/ndnsf-change-audit-20260916-temOPI/`。
本轮普通checkpoint再次被全index扫描拒绝，首个命中仍为
`.specify/memory/constitution.md:16`；日志保留于上述目录`checkpoint.log`。
38个论文文档路径暂存待提交；未生成commit，未绕过、不计作实验结果。

## 2026-09-16 — Proposal coverage checkpoint blocked

四入口论文构建、保留内容检查及PDF审查通过后，普通checkpoint仍被全index引用扫描拒绝。
首个命中`.specify/memory/constitution.md:16`，未生成commit，未绕过hook。
日志`/tmp/ndnsf-proposal-coverage-20260916-sV2398/checkpoint.log`；持久范围/结果见
[coverage revision](PAPER/proposal-defense/coverage-restoration-20260916.md#checkpoint-boundary)。
这不是产品或文档编译失败；文档已保存，源码/PDF明确暂存待提交。

## 2026-09-16 — Proposal coverage source-query option

CodeGraph 首次 explore 因不支持 `--max-nodes` 在参数解析阶段退出；help 确认
`--max-files` 为该版本接受的边界选项。未启动产品测试，后续使用正确参数。
见 [coverage revision](PAPER/proposal-defense/coverage-restoration-20260916.md#tool-boundary)。

## 2026-09-16 — Optional proposal comparison helper environment

本轮摘要 checkpoint 也被既有 pre-commit 全 index 引用扫描拒绝，首个命中
`.specify/memory/constitution.md:16`；日志 `/tmp/ndnsf-abstract-20260916-r1/checkpoint.log`。
未绕过钩子，已验证的文档保持待提交。

摘要修订期间试探 `build_pdf_comparison.py --help`，系统 Python 因
`ModuleNotFoundError: No module named 'fitz'` 在 import 阶段退出，未触碰对照 PDF。
未重试该额外转换；本次采用已存在的 LaTeX/Poppler 工具完成摘要构建和渲染。
精简记录及后续环境入口见 [abstract review](PAPER/proposal-defense/abstract-review-20260916.md#validation)。
这是可选文档工具依赖边界，不是论文编译或产品验证失败。

## 2026-09-16 — Spec187 r50 MiniNDN startup environment boundaries (RESOLVED)

r50 第一次重放在 MiniNDN 启动前被 `STATE_ROOT_OWNER_MISMATCH` 拒绝；修正为
root-owned state/output 后，最小化 sudo 环境缺少 `SHELL`，MiniNet 的
`popen(shell=True)` 控制进程得到 `KeyError`。两次都没有进入 ACK、Selection 或
Provider，请求链不计失败。原始日志和输出分别保留在
`.codex-tmp/spec187-local-yolo-r50.log`、`results/spec187-local-yolo-r50/`。

补齐完整运行环境后，r51 在同一依赖身份和 candidate-bound 输入上完成 Y-A，
`SPEC180_CASE_RESULT status=PASS`，终端响应与 7,267-byte native result 已观察；
详见 [current-source local confirmation](../specs/187-yolo-minindn-sif-app/evidence/b187-local-yolo-recheck-20260916.md#2026-09-16-current-source-local-miniindn-confirmation)。

## 2026-09-16 — Spec187 segmented-input selector used stale NDN-SVS runtime (RESOLVED)

直接运行 `RequestScopedSelection/SelectedProviderReceivesSegmentedEncryptedInput`
时，命令未将匹配的 `/home/tianxing/NDN/ndn-svs/build-spec187-local` 放在动态库
搜索路径首位，`ldd` 实际解析到旧的 `/home/tianxing/NDN/ndn-svs/build`；测试夹具
因此在请求发布边界失败，未产生协议结论。原始日志为
`.codex-tmp/spec187-segmented-input-r1/selector-rerun-20260916.log`。

修正库搜索顺序后同一 C++ selector 返回 `rc=0`、`No errors detected`，日志为
`.codex-tmp/spec187-segmented-input-r1/selector-rerun-20260916-matched.log`。
该边界属于运行时依赖身份选择，不能归因于输入分段实现。

## 2026-09-16 — Spec187 local MiniNDN dependency identity boundary (RESOLVED)

重新生成 native receipt 后，运行时探针首先在 Python import 阶段发现框架要求的
`subscribeToProducerWithCatchUp` 未由 Waf 选择的旧 NDN-SVS 二进制导出；这是
头文件/运行库不一致，未启动 MiniNDN，不能算协议结果。原始构建与失败边界保留于
`.codex-tmp/spec187-framework-svs-reconfigure-20260916-r1-build.log`。

使用同一 SVS 源码 `9f2d8a4`、本机 Boost 1.71/ndn-cxx 重新构建
`build-spec187-local`，重新配置 Spec187 受影响目标并以 `-j4` 完成 `205/205`
（12m2.718s）；Python binding 与 native identity verify 均通过。修复后的实际
`libndn-svs`、`libndn-cxx` 和 `libnac-abe` 路径与摘要见
[B187 dependency-aligned replay](../specs/187-yolo-minindn-sif-app/evidence/b187-local-yolo-recheck-20260916.md#2026-09-16-c-svs-dependency-aligned-minindn-replay)。

在新依赖身份上，当前源码 MiniNDN `Y-A` 两次独立运行均返回
`SPEC180_CASE_RESULT status=PASS`，C++ selector 写出 terminal
`SPEC187_NATIVE_REQUEST_PASS`；这两次结果不改变仍未执行的 SIF/APP、negative path
和 Tiger 资格状态。

## 2026-09-16 — UAV editable export checkpoint hook boundary

四页 PPTX 导出、文本/对象检查和全页 LibreOffice/Poppler 渲染通过后，普通
`git commit` 被既有 `.git/hooks/pre-commit` 拒绝（exit 1）：
`Commit blocked: development-assistant files or references remain in the Git index.`
钩子默认扫描整个 index，既有 `.specify/memory/constitution.md:16` 等引用触发拒绝。
未重试、未绕过钩子，交付文件保留待提交；不是产品或 PPTX 验证失败。
精简证据及产物摘要见 [UAV export review](NDNSF-UAV/slides/UPDATES_UAV-review.md#editable-powerpoint-export--2026-09-16)。

## 2026-09-16 — Spec187 authority handoff source closure

新的 authority source handoff 首次使用现有依赖工作区时，在 `HANDOFF_SOURCE_UNTRACKED:examples/example-trust-anchor.cert` 处拒绝，未创建 bundle、未启动构建。该文件是依赖 checkout 的本机生成身份资料；原始记录 `.codex-tmp/spec187-authority-20260916/prepare-r1.log` 与 `prepare-r1.failure.json` 保留。改用三个锁定 revision 的干净 detached worktree 后，source handoff 成功；不放宽 untracked-source 门。

## 2026-09-16 — Spec187 authority definition render path

authority handoff 成功后，首次 render 传入相对 bundle 路径，被 `HANDOFF_BUNDLE_PATH_NOT_ABSOLUTE` 在构建前拒绝；未生成 definition、未启动 Apptainer。原始记录 `.codex-tmp/spec187-authority-20260916/render-r1.log` 与 `render-r1.failure.json` 保留；改用绝对路径后 render 成功。

## 2026-09-16 — Spec187 inherited DI headers differ from compiled source

`unit-r2` 首个编译错误为 `RedistributionSpec` 未声明：pkg-config 指向 current/include，那里仍是父镜像遗留 DI 头；本次 builder 只安装 Core headers，DI 新头仅在 sealed replay 中。修复正式安装清单，删除遗留 DI header tree，并在 native verifier 比较已安装头与 replay 的完整集合及字节。不通过调整 include 优先级隐藏问题。r12 SIF 保持 BUILT_UNQUALIFIED；日志 `unit-r2/run.log`，见 [candidate evidence](../specs/187-yolo-minindn-sif-app/evidence/b187-complete-candidate.md)。

## 2026-09-16 — Spec187 container unit consumer compile flags

最终 SIF 隔离 native probe PASS；`unit-r1` 的 C++ fixture 编译因手写 flags 缺少 NAC-ABE CMake 配置宏而找不到 `nac-abe-config.hpp`。修正测试消费者使用镜像内 pkg-config 的 cflags/libs，不重编生产库。原始 `unit-r1/run.log` 与 FAIL record 保留；见 [candidate evidence](../specs/187-yolo-minindn-sif-app/evidence/b187-complete-candidate.md)。

## 2026-09-16 — Spec187 sandbox packing requires fakeroot

r11 SDK/native 预检通过，sandbox 复制时发现三个容器 UID 私有运行目录 Permission denied；已主动中止打包，未接受可能遗漏目录的镜像。恢复入口须显式 `build --fakeroot`，与原 definition 构建的 UID 映射一致，不修改运行目录权限绕过。日志 `build-r11/build.log`；见 [candidate evidence](../specs/187-yolo-minindn-sif-app/evidence/b187-complete-candidate.md)。

## 2026-09-16 — Spec187 SDK probe scope after runtime environment repair

r10 恢复预检在 `SDK_WRONG_ORIGIN:/opt/ndnsf-di/current/lib/libopenabe.so` 停止，尚未打包。修复后的默认 runtime 优先项目库，SDK-only verifier 因同名库来自项目层而拒绝。只为 SDK probe 设置 base/ORT 库路径；最终 native probe 继续检查默认环境，不放宽来源断言。日志 `build-r10/build.log`，见 [candidate evidence](../specs/187-yolo-minindn-sif-app/evidence/b187-complete-candidate.md)。

## 2026-09-15 — Spec187 inherited SDK environment shadows repository runtime

r9 恢复入口 SDK verifier PASS 后，默认 clean container 环境导入 native binding 失败：继承的 `91-sdk-environment.sh` 覆盖了 90-environment 中的 NDNSF 路径，Core/DI DSO 找不到。此前 final post 的显式 export 掩盖了启动环境问题。日志 `build-r9/build.log` 保留；正式模板增加后置 92 repository 环境文件，并对当前 final rootfs 应用同一环境增量、复验默认启动，不重编原生代码。见 [candidate evidence](../specs/187-yolo-minindn-sif-app/evidence/b187-complete-candidate.md)。

## 2026-09-15 — Spec187 final SIF packing disk peak

标准 r8 的完整 builder 与 final post 均通过；最终 SIF 创建在 `copy_file_range` 报 `no space left on device`，rc=255。空间估算漏计压缩 squashfs 与 SIF 输出同时存在的峰值；不属于编译/模型/协议失败。保留完整 final rootfs 与 `build-r8/build.log`，仅恢复最终封装，并复验同一 definition/source seal/SDK/native manifest；不重编已通过的生产库。见 [candidate evidence](../specs/187-yolo-minindn-sif-app/evidence/b187-complete-candidate.md)。

## 2026-09-15 — Spec187 recovered builder final-delivery boundary

r7 Repo wheel、builder import/ABI/loader 检查通过；但把 retained rootfs 中产物映射为 final `%files` 的恢复 definition 被正常入口以 `WRONG_BUILD_BOUNDARY_HOST_BINARY_INPUT` 拒绝（rc=4），未启动 SIF 封装。不得放宽此门禁或把准备层审查 PASS 当交付 PASS。保留 builder 产物、原始日志和恢复 recipes，使用已修复的正式两阶段模板重新构建 NDNSF 层；封存 base 不重建。见 [candidate evidence](../specs/187-yolo-minindn-sif-app/evidence/b187-complete-candidate.md)。

## 2026-09-15 — Spec187 Repo binding library-directory contract

retained builder r6 的 C++ 293 steps 通过（12m56.967s），`ndnsf` Python wheel 构建安装通过。Repo binding metadata 拒绝 `NDNSF_LIBRARY_DIR` 中不含 Core 的 `/opt/ndn-base/lib`；其 setup.py 要求每个显式目录均包含 Core。改为仅显式传 `/opt/ndnsf-stage/lib`，外部依赖仍由 pkg-config/pinned prefix 提供。保留 `build-r6/builder.log`，从 Repo binding 继续，不重编 Core/DI。见 [candidate evidence](../specs/187-yolo-minindn-sif-app/evidence/b187-complete-candidate.md)。

## 2026-09-15 — Spec187 consumer source archive missing DI metadata template

build-r3 通过 SDK verifier、外部依赖 configure 和 Rust tokenizer 编译后，Waf 因源码包缺少 `NDNSF-DistributedInference/ndnsf-distributed-inference.pc.in` 停止（rc=255），尚未开始 C++ 编译。保留 rootfs 与原始 `build-r3/build.log`；补齐源码封存范围后复用现场，不重建 base。见 [candidate evidence](../specs/187-yolo-minindn-sif-app/evidence/b187-complete-candidate.md)。

## 2026-09-15 — Spec187 consumer temporary directory cleanup

build-r2 通过封存 base 的 SDK verifier 后，在清理固定 `/tmp/nac-abe-build` 等历史目录时权限拒绝，未开始 NDNSF 编译；不是模型或协议失败。按用户指示暂停重试先清理磁盘，之后修复构建临时目录隔离。见 [candidate evidence](../specs/187-yolo-minindn-sif-app/evidence/b187-complete-candidate.md)。

## 2026-09-15 — Spec187 consumer definition render preflight

`bundle-r4` 封存成功后，render 的相对 bundle 路径被 `HANDOFF_BUNDLE_PATH_NOT_ABSOLUTE` 拒绝，尚未构建。改用绝对路径，保留 `.codex-tmp/spec187-app-build-20260915/render-r4.log`；见 [candidate evidence](../specs/187-yolo-minindn-sif-app/evidence/b187-complete-candidate.md)。

## 2026-09-15 — Spec187 base SDK probe include closure

外部依赖实际编译安装完成后，SDK C++ probe 缺少 `/opt/ndn-base/include/nac-abe`，`common.hpp` 无法解析。`images/base-sdk-20260915-r1/build.log`、FAIL record 与 rootfs 保留；修复并复审后复用该现场验证，不将编译成功当 SDK PASS。见 [完整候选证据](../specs/187-yolo-minindn-sif-app/evidence/b187-complete-candidate.md)。

更新：r5 只读审查通过；保留 rootfs 内 C++/Rust/Python/ELF 全部 probe PASS。consumer 脚本测试另有旧 handoff 断言仍要求复制外部源码，45 passed / 1 failed，按新的两层契约修正后复审复测；未产生协议结论。

## 2026-09-15 — Spec187 complete candidate NDNSD metadata check

首轮完整容器构建中 ONNX、NAC-ABE、NDN-SVS、NDNSD 编译安装成功，随后旧脚本将空的 `pkg-config --cflags-only-I ndnsd` 判为路径错误。目录已在 `CPLUS_INCLUDE_PATH` 时该输出会被过滤；修复为精确核对 `--variable=includedir`，保留 libdir 检查。原始 `build-r1/build.log` 保留，候选未生成，非协议失败。见 [完整候选证据](../specs/187-yolo-minindn-sif-app/evidence/b187-complete-candidate.md)。

## 2026-09-15 — Spec187 offline Cargo vendor preparation

实际 native input prepare 首边界为旧 Cargo cache 缺少锁定的 `wasi v0.11.1+wasi-snapshot-preview1`，offline vendor rc=101。已保留 FAIL record 与原始 `native-inputs/vendor.log`；不改 lock，在宿主准备阶段补全锁定缓存后，用全新目录重新离线封存。未开始 SIF/C++ build，也不是协议失败；见 [完整候选证据](../specs/187-yolo-minindn-sif-app/evidence/b187-complete-candidate.md)。

更新：缺项已按原 lock 补齐，offline `native-inputs-r2` 成功。后续 handoff-r1 在依赖工作区的未跟踪 `examples/example-trust-anchor.cert` 处拒绝；使用锁定提交的干净依赖 worktree 重试，保留该原始边界而不放宽源封存规则。

## 2026-09-15 — YOLO base CPU smoke preparation (RESOLVED)

首次手动预检错误地将 oracle digest 与 NPY 的裸 payload 比较；exporter 实际绑定完整 NPY，按源码核对完整文件后通过。静态 r1 另要求执行入口绑定模型/weights/fixture/oracle/SIF 摘要和硬超时；r2 修正后 `STATIC_PASS`，容器内 C++ 真实 YOLO 三次推理通过独立 oracle。全零 SIF hash 负例在容器启动前拒绝。仅 `YOLO_CPU_MODEL_SMOKE_ONLY`，不代表最终 NDNSF+APP 或 MiniNDN；原始 `.codex-tmp/yolo-sif-smoke-20260915/` 与 [持久证据](../specs/187-yolo-minindn-sif-app/evidence/yolo-base-cpu-20260915.md)。

## 2026-09-15 — Tiger layered APP runner static boundary (RESOLVED)

官方 `review-agent` 首轮复核发现配对运行器的身份 TPM 目录类型、复制后 `tpmInfo`
locator、NFD socket 子项和 scratch cleanup 边界问题；修正后又发现整个 NFD 父目录可被
任意传入，最后发现 Slurm job scope 检查拒绝 job 根目录。当前版本按单角色目录复制并同步
locator，要求 NFD 父目录由当前用户拥有、仅 owner 权限、位于解析后的 job/local scope，且
只包含目标 socket，并在容器执行前复核 socket device/inode。三次冻结快照最终
`STATIC_PASS`，无 P0--P3；`bash -n`、ShellCheck、TigerCluster 离线测试为
`81 passed, 1 skipped`。详见
[Tiger pair runner evidence](../specs/185-prepared-model-runtime/evidence/tiger-pair-runner-20260915.md)。
本项仍未观测 regular base SIF、真实 Apptainer/KeyChain/NFD、C++ 请求链或 Tiger/Slurm；
此前 `APP_BASE_SIF_PATH_SYMLINK` 和 SSH alias 无法解析的本地边界保持原记录。

## 2026-09-15 — Tiger baseline role KeyChain locator boundary (RESOLVED)

静态复审首先发现普通角色没有显式 paired KeyChain locator；首个修正又把
`pib-sqlite3`/`tpm-file` 写成文件路径，并让 issuer `prepare` 继承运行 locator。
按 ndn-cxx backend 契约改为目录级 `/identities/<role>/.ndn`，并在 `prepare`
阶段省略两个变量，保持每个角色和 issuer 的 store 隔离。官方 review-agent
对冻结快照 `09e7ccd0cb8fd827e8fd9a94fc1fc8dcf8710c345558898921234794934e5d7e`
返回 `STATIC_PASS`；baseline 聚焦测试 53 项通过。此项尚未运行 SIF、NFD、C++
请求链或 Tiger qualification，详见
[Tiger baseline identity evidence](../specs/185-prepared-model-runtime/evidence/tiger-baseline-identity-20260915.md)。

## 2026-09-15 — Spec185 T013 mixed external-binary closure (RESOLVED)

The first final normal run rebuilt only `spec185-process` after a public native runner ABI
repair. The external `DI_NativeRequester` and prepared selectors remained stale; the first
requester boundary was an empty-log `-11`, before any protocol result. The raw run is
`.codex-tmp/spec185-t013-final-20260915/process-runtime-normal.log`. Rebuilding the complete
DI/process/prepared-selector and external caller closure produced the final normal matrix
`RC=0`; this failure is a build-identity boundary, not a native protocol result.

## 2026-09-15 — Spec185 T013 leak-enabled sanitizer dependency boundary (QUALIFIED)

The ASan/UBSan process matrix with leak detection enabled stopped with `RC=201` in
`/usr/local/lib/libopenabe.so` policy-tree allocations (1,232–1,236 bytes across 16 allocations).
No ASan/UBSan memory or undefined-behavior report was emitted. The same complete candidate with
`detect_leaks=0` passed all five C++ process cases twice, and the isolated drain selector passed
twice. This remains an external dependency limitation rather than a product sanitizer PASS;
the raw runs are `.codex-tmp/spec185-t013-final-20260915/process-runtime-asan.log` and
`process-runtime-asan-final.log`, with the durable decision in
[B7 final candidate convergence](../specs/185-prepared-model-runtime/evidence/b7-cpp-qualification.md#b7-final-candidate-convergence-20260915).

## 2026-09-15 — Spec185 T021 protected Provider runner-address ABA (RESOLVED)

The first production protected independent-grant selector reached both grant verification and
assembly/source-fetch boundaries but reported `runnerRuns=1` for two newly-created runners. The
ProviderRoleWorker exact-forward cache used a raw runner address as identity; allocator reuse returned
the first runner's cached output to the second request. The raw failure is preserved at
`.codex-tmp/spec185-t021-runtime/production-independent-grants-normal-v4.log`. A monotonic identity in
an external process-local registry, removed at base destruction, fixed the boundary without changing
the public polymorphic base layout. The v9 static review, normal selector, and ASan/UBSan selector now
pass; durable details are in [T021 lifecycle evidence](../specs/185-prepared-model-runtime/evidence/b7r-lifecycle-fixes-20260915.md#t021-follow-up-production-protected-independent-grant-matrix).

## 2026-09-14 — Side-by-side comparison layout and raster checks (RESOLVED)

首轮说明页标题的PyMuPDF textbox高度校验失败；r2/r3修复后，微小光栅字形差异与一级灰度取整触发像素门禁。保留三轮日志与候选；全量只读诊断后，r4的79页／130面板文本精确校验、显著光栅差异容差和全页渲染通过，不声明逐像素相等。见[side-by-side evidence](../specs/185-prepared-model-runtime/evidence/proposal-side-by-side-20260914.md)。

## 2026-09-14 — Proposal comparison guide pagination (RESOLVED)

首轮PDF对比稿说明页排成两页，说明中的固定页码偏移一页；候选未晋升。保留 `r1/` 全部产物，新增单页说明门禁后修复；最终 `r3/` 110页渲染、109页源内容保留和页码检查通过。不属于协议或实验结果。见[comparison evidence](../specs/185-prepared-model-runtime/evidence/proposal-pdf-comparison-20260914.md)。

## 2026-09-14 — Proposal timeline wording overflow (RESOLVED)

Proposal定位修订首轮slides时间线页出现6.76622pt的vbox溢出；失败PDF未晋升。压缩February 2027行后，独立`latex-r2/`目录八入口构建及49页PPTX回读通过，原始日志保留；不属于产品或实验结果。详见[document evidence](../specs/185-prepared-model-runtime/evidence/proposal-structure-20260914.md)。

## 2026-09-14 — Proposal document layout/export boundaries (RESOLVED)

The advisor-comment revision first produced a 24.76523pt overfull slide and then
hit the PPTX exporter's approved-build-root guard. A compact four-message figure
and an isolated directory under `slides/build/` resolved those document-tool
boundaries; no exporter guard was bypassed. The first layout log was superseded
by an incremental build, so only its exact tool-reported first error is retained;
the failed exporter log and final r3/r4 build logs remain separate. This is not a
native/runtime/protocol result. See
[documentation checkpoint](../specs/185-prepared-model-runtime/evidence/proposal-advisor-review-20260914.md)
and [final document checks](PAPER/proposal-defense/advisor-review-validation-20260914.json).

## 2026-09-14 — Spec185 B6 T011 requester PIB/TPM identity boundary

The first post-readiness-repair C++ unary attempt (`unary-v4`) reached the
Runtime requester route after Controller, grant authority, and Provider startup,
but the permission response failed at production decryption:
`Cannot decrypt PermissionResponse AES key with local KeyChain`. The
Controller encrypted for the requester certificate in the process PIB, while
Runtime had created a different `pib-memory:` certificate; the process then
ended at `NATIVE_REQUEST_BOOTSTRAP_TIMEOUT`. No ACK, Selection, or Provider
protocol result was observed. Raw evidence is retained at
`.codex-tmp/spec185-b6/unary-v4/`. The v12 candidate now reuses the paired
`NDN_CLIENT_PIB`/`NDN_CLIENT_TPM` stores and rejects a partial pair, with the
memory KeyChain fallback preserved for isolated callers. A fresh `-j4` build
and unary/stream process attempts are required; this is an authorization
bootstrap boundary, not a protocol PASS.

## 2026-09-14 — Spec185 B6 T011 launcher library-path boundary

Fresh unary attempt `unary-v5` stopped during Python launcher import before
Controller, authority, Provider, or requester startup. The command used an
invalid repository-relative NAC-ABE path in `LD_LIBRARY_PATH`; `_ndnsf.so`
loaded the candidate Core library and then failed on missing
`ndn::nacabe::Consumer::clearCache`. Raw launcher logs are retained under
`.codex-tmp/spec185-b6/unary-v5/`. This is a startup environment failure, not
a protocol result; the next attempt uses
`/home/tianxing/NDN/nac-abe-integration-182/install-spec184-r4/lib`.

## 2026-09-14 — Spec185 B6 T011 requester authorization bootstrap boundary

The first fresh C++ unary process attempts (`unary-v2` and `unary-v3`) started
the Controller, grant authority, and Provider successfully, then reached the
new `Runtime.open -> User.prepare -> PreparedModel.request` route.  The first
request boundary was the production `ServiceUser` admission check: NAC-ABE
decryption was still pending and no Runtime-owned user permission fetch had
been issued, so `BeginCollaborationWithProviders` rejected the request before
ACK/Selection.  The requester returned `NATIVE_REQUEST_BEGIN_FAILED`; this is
not a Provider/protocol result.  Raw run roots are retained at
`.codex-tmp/spec185-b6/unary-v2/` and `.codex-tmp/spec185-b6/unary-v3/`, with
launcher records `unary-v2.log/.rc` and `unary-v3.log/.rc`.  Spec185 B6 v10
adds the controller permission bootstrap and a bounded C++ readiness retry;
a new static review and fresh process run are required before this boundary
can be considered resolved.

## 2026-09-13 — Spec185 B3 T005 native selector bootstrap boundary

The B3 full C++ selector rebuilt successfully, but the first runtime test
started a real Runtime Face without an Attribute Authority route.  NAC-ABE's
constructor-time public-parameter fetch reached its retry limit and threw
`Failed to fetch public parameters after multiple attempts.` on the owned IO
thread; the selector returned `rc=201` and the later SIGABRT/SIGSEGV reports
were cascade failures from aborted fixtures, not independent protocol results.
Raw output is retained at
`.codex-tmp/spec185-t005-full-v3-normal-20260913T0550/run.log` with build
status in the same directory.  The fixture was first tightened to cancel its
handles, but the next rerun showed that `Runtime::close()` still left the
Face/io_context dispatching that unowned retry during `drain()`.  A proposed
eager `io.stop()` at the close fence was rejected by static review because it
could discard queued `CancelCollaboration()`/scope cleanup callbacks.  The
current production fix keeps close/drain ordering intact and catches an
exception escaping the owned Face thread, records an `ioFailed` lifecycle
signal, and rejects later client materialization instead of calling
`std::terminate`.  This does not weaken the public-parameter readiness gate.
A new static review and selector run are required.

## 2026-09-12 — Apptainer host upgrade boundaries

Official1.5.3deb的AppArmor ABI3占位profile无法由Ubuntu20.04的2.13.3 parser加载；已备份并做无profile兼容处理，postinst复验通过，未关闭系统AppArmor。历史r119镜像链接目标不存在，首次exec止于路径检查；改用独立最小SIF构建及普通/root执行均通过。无NDNSF协议/GPU/Tiger结论。见[持久记录与原始日志路径](../Experiments/TigerCluster/docs/apptainer-153-upgrade-20260912.md)。

## 2026-09-12 — Spec185 B0C TSan harness boundary

The first B0C TSan selector stopped with exit 66 in
`PendingReaderCompletionRetiresItsTimerBeforeDrain`. The first boundary was
the test harness: an asynchronous worker executed Boost.Test assertions while
the main test thread was also using the framework. The stack did not establish
a product data race. The raw run and isolated reproduction are retained under
`.codex-tmp/spec185-b0c-runtime-20260912/tsan-build-v28.log` and
`.codex-tmp/spec185-b0c-runtime-20260912/tsan-isolate-pending-completion-v28.log`.
The callbacks were changed to promise/atomic handoff, statically re-reviewed,
and the Core selector then passed twice under TSan; see
[B0C evidence](../specs/185-prepared-model-runtime/evidence/b0c-core-operation.md).

The whole-tree Waf install attempt was intentionally interrupted in its
post-install pip phase after the bounded Core library install. It is an
installation-scope boundary, not a product failure; the Core-only staged
consumer is recorded separately in the same B0C evidence.

## 2026-09-12 — Spec185 B0 installed-consumer packaging boundaries

The first B0 external consumer attempt correctly stopped on a source-tree
include leak because its temporary prefix was under `.codex-tmp/`. After moving
the candidate outside the repository, the installed header matrix exposed two
packaging metadata gaps: the Core package did not export the NAC-ABE generated
header directory, and third-party NDN-SVS headers triggered `-Werror` warnings
when advertised as ordinary includes. The next attempt reached the disabled
consumer link and found an unused ONNX fixture under the disabled build. These
are preserved installation/test-harness boundaries, not protocol results. Raw
runs remain under `.codex-tmp/spec185-b0-external-consumer-20260912.log`,
`...-r2-20260912.log`, `...-r3-20260912.log`, and
`...-r4-20260912.log`; the corresponding fixture fix is under review before
the next matrix retry.

## 2026-09-12 — Spec184 native MiniNDN caller route preflight

The first focused backend-registration rerun used the historical default
`build-system-j2/examples/di-native-provider` path and stopped before starting
any Provider because that executable is absent. No protocol, model, or native
request result was observed. The raw output is retained under
`.codex-tmp/spec184-native-route-20260912/default-path.log` (SHA-256
`62f15a4d8f280a337dfd54b7b11627fa38997349b02221c9ae6a24624257b941`). The
same suites were then rerun against the explicit current candidate Provider
path and passed; see [post-ACK caller routing](../specs/184-native-di-closure/evidence/native-minindn-post-ack-routing-20260912.md).

## 2026-09-11 — Proposal Origin expansion build path

首次文档构建驱动将相对输出目录传入改变 cwd 的 latexmk，导致预期 `main.log` 缺失；不是产品协议失败。修复为绝对路径并保留 r2 输出。证据：[proposal expansion](../specs/184-native-di-closure/evidence/proposal-origin-expansion-20260911.md)。

## 2026-09-11 — Proposal application-validation notes layout

The first document check found an overfull generated notes line after the new
DI table used `input/result`. The source now uses `input and result`; rebuilt
notes pass the layout check. Active Context Mode tasks hashes were stale; actual files supplied
the authority fallback. No product test ran. See
[application-validation audit](PAPER/proposal-defense/research-revision-audit.md)
and `.codex-tmp/proposal-app-validation-20260911/notes-first.log`.

## 2026-09-11 — Proposal email-alignment retrieval boundary

Active Context Mode health rejected stale Spec184 plan/tasks hashes; no stale
search result was used as authority. Project health passed and actual files
supplied the fallback. No product test ran. See
[email-alignment audit](PAPER/proposal-defense/research-revision-audit.md)
and `.codex-tmp/proposal-email-alignment-20260911/context-health.json`.
The first wording scan matched a suffix across table cells; the audit records
the word-boundary correction and preserved `verify-first.log`.

## 2026-09-11 — Proposal terminology retrieval guard boundary

Context Mode rejected a historical timeline query before execution because its
source/category did not meet the guard. No returned history was treated as
authority; project health passed and current repository files supplied evidence.
No product test failed. See [terminology audit](PAPER/proposal-defense/research-revision-audit.md)
and `.codex-tmp/proposal-nonce-challenge-20260911/retrieval-boundary.md`.

## 2026-09-11 — Proposal Reason 3 heading-check boundary

Four paper builds passed, but the PDF checker counted an explanatory reference
to `Reason 2:` as a second heading. Matching the actual heading text fixes this
validator boundary. No product test ran. See
[Reason 3 audit](PAPER/proposal-defense/research-revision-audit.md) and
`.codex-tmp/proposal-reason3-20260911/first-check.md`.

## 2026-09-11 — Proposal DNMP bibliography mirror boundary

The first document validation rejected root/English-entry text inequality:
the DNMP DOI was added to the root `ref.bib` but not its independent `en/ch`
copies. Synchronizing the three entries and rebuilding fixes the metadata
boundary. Earlier slide overflow and Chinese sparse pagination were also
corrected; no product test or experiment ran. See
[DNMP throughline review](PAPER/proposal-defense/dnmp-throughline-review-20260911.md)
and `.codex-tmp/proposal-dnmp-throughline-20260911/validation-first-pass.log`.

## 2026-09-11 — Proposal scope checkpoint hook boundary

The default commit hook rejected repository-wide historical assistant references
before creating a commit. The hook and maintained Spec182 evidence document
`NDNSF_LOCAL_CHECKPOINT=1` for local documentation checkpoints; that mode retains
prohibited-path checks. No hook is changed and no `--no-verify` is used.
See [scope review](PAPER/proposal-defense/invocation-scope-review-20260911.md)
and `.codex-tmp/proposal-invocation-scope-20260911/checkpoint-hook-default.log`.


## 2026-09-11 — Proposal document build invocation boundary

The sentence-review build first stopped before LaTeX execution because its
output directory was created relative to the document cwd while redirection
used the repository's absolute temporary path. Recreating explicit absolute
output directories resolved this invocation error. No product or protocol test
ran at that boundary. The source patch also initially rejected a mismatched
Chinese context line without applying edits; exact-context retry succeeded.
The final PPTX attempt rejected a `final-build` leaf name under the converter's
existing safe-directory rule before clearing/generating anything; the retry
uses the permitted `ndnsf-final-build` name without weakening the guard.
See [document review](PAPER/proposal-defense/sentence-review-20260911.md) and
`.codex-tmp/proposal-sentence-review-20260911/` for the subsequent document checks.

## 2026-09-11 — Spec184 C++ epoch-input mapping boundary

The current candidate was rerun with the parent-qualified selector
`Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI01OneProvider`, native timing, and
trace logging. Both attempts stopped at the same first production boundary
(exit `201`) before ONNX execution: `NDNSF_DI_EPOCH_COORDINATOR phase=epoch_start`
was followed by `native epoch coordinator is missing its canonical token input`.
The collector's later `stream event gap exceeded retry budget` is therefore a
downstream observation, not the first protocol boundary. Raw runs remain under
`.codex-tmp/spec184-tiny-i01-diagnose-20260911/` and
`.codex-tmp/spec184-tiny-i01-diagnose-20260911-r2/`; the second trace log SHA-256
is `d07c4009a28e6c7df4ce0e0b4b2c105f62dbf0b29a9390aefadcc911267be9c1`.
The first trace did not prove whether the projection mapping was lost. A
default-off handler trace in rerun `r3` records
`input_count=2 application_input=true edge=APPLICATION_INPUT@request-input
edge=TOKEN_FEEDBACK@group-spec175-i01`, proving that the signed projection
reaches the handler correctly. The remaining boundary is the request-input
payload or its handoff into `NativeEpochCoordinator`; the next repair must
identify why the encoded `input_ids` bundle is absent at `tokenIdsFromInputs`
before another qualification retry. The `r3` raw log SHA-256 is
`637884258ff60887e4a64b26c98f03a887fab4f81ac7b09bddc9906614cb33e2`. See
The follow-up `r4` run added payload diagnostics but still emitted no
`NDNSF_DI_INPUT_BUNDLE` record before the coordinator failure; its raw log
SHA-256 is `49a8323c50da10f4c6655b4d45c7f3233f6cb294f2e3d926c2f140811253d9f7`.
See
[`T007 qualification evidence`](../specs/184-native-di-closure/evidence/t007-current-native-qualification-20260911.md).
The production fixes were then rerun as `r6`: the same I01 selector exited
`0`, reached epochs 0 through 7, emitted eight token events, and produced a
terminal `NDNSF-DI-FINAL-V1` EOS payload. The raw log SHA-256 is
`4c4015b900e2ccf1f5f7706fe3622a1d7e1aa68a11be82ae84c23c778eae850e`.
This closes the I01 first-boundary defect only; remaining I02+ matrix
selectors still need the bounded C++ dynamic loop. See
[`T007 qualification evidence`](../specs/184-native-di-closure/evidence/t007-current-native-qualification-20260911.md).

The parent-qualified C++ batch `Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnx*`
was then rerun against the repaired candidate and exited `0` with no Boost
errors. Positive I01/I02/I03/I04/I05/I06/I11/I12/I15/I16 cases completed their
native stream/replacement oracles; negative I07/I09/I10/I13 cases reached and
asserted their declared failure boundaries. Raw output is retained at
`.codex-tmp/spec184-tiny-batch-20260911/run.log`, SHA-256
`29773f5d89ce9192bf18be80080bf3a08acf6dd7165bcf7ab78f51326cea43ef`.
This closes the sampled Spec175 C++ behavior classes only; it does not close
process/no-Python, parser-fuzz, candidate refresh or inherited qualification.

The rebuilt candidate then ran the complete C++ integration and unit
executables with candidate-first libraries. Both exited `0` with
`*** No errors detected`: integration log
`.codex-tmp/spec184-full-integration-20260911-r2/run.log` has SHA-256
`8b74652a9a9e13aec564e1bb106dca1bf2a724c7a4d84ca00a1209709ad910c8`, and unit
log `.codex-tmp/spec184-full-unit-20260911-r2/run.log` has SHA-256
`16037133632b55d5894b88672fd2d32598955822aab9f82b2e90d1751e3910ed`.
This closes the complete local C++ unit/integration sweep for this source
tree; process/no-Python, parser-fuzz, fresh candidate convergence and external
SIF/Tiger rows remain open.

The same `Spec175NativeTinyOnnx*` behavior-class batch was run in the rebuilt
ASan/UBSan tree with unsuppressed leak detection. The selector assertions
printed their normal result lines, but process exit was `134` because
LeakSanitizer reported `SUMMARY: AddressSanitizer: 3096985 byte(s) leaked in
25092 allocation(s)`. No use-after-free, buffer, or undefined-behavior report
preceded the leak summary. Raw output is retained at
`.codex-tmp/spec184-tiny-asan-20260911-r1/run.log`, SHA-256
`827b56f8c870f675fc7b8c9ee14113e743bc50199e966593c63ab069298da89f`.
This is a sanitizer `DYNAMIC_FAIL` for the repeated multi-environment batch;
normal C++ remains `PASS`, and sanitizer qualification now requires a bounded
single-case rerun and leak ownership classification.

The bounded ASan/UBSan I01 rerun exited `0` with eight events, final EOS, and
no sanitizer diagnostics; raw log `.codex-tmp/spec184-tiny-asan-20260911-r2/run.log`
has SHA-256 `34d9169715b04216a001050a54a85435062407de9726f4507198915d7cdde40c`.
The two-provider I02 case passed its C++ assertions but exited `134` under leak
detection with 87,522 bytes in 720 allocations; raw output is retained at
`.codex-tmp/spec184-tiny-asan-20260911-i02/run.log`, SHA-256
`9171b9e06666a56fb5eae7d9309feabe804492b895dbebef1896860c2018b9d7`.
The leak stack is rooted in existing `makeD2bCoordinatorOptions` test callback
captures, so I02 remains a sanitizer `DYNAMIC_FAIL` pending explicit fixture
cleanup or ownership classification.
After repairing the alias collision, rerun `r5` confirmed the bundle is
present and decodes as `tensors=input_ids`, then reached the next C++ boundary:
`NativeProviderRuntime requires a runner preparation callback`. This is the
preassembled compatibility path entering the epoch coordinator without a
preparation factory; its raw log SHA-256 is
`61a75cf871432eb57158a9dc07de05cd3e787aa96873bdb5b7ee46b44dd97744`.
See
[`T007 qualification evidence`](../specs/184-native-di-closure/evidence/t007-current-native-qualification-20260911.md).

## 2026-09-11 — Spec184 T007 no-Python harness preflight boundary

The candidate-bound `run-spec182-native-closure.py` invocation for `I01` stopped
before process setup with exit `2` and `manifest schema mismatch`. The frozen
`tests/fixtures/spec182/case-manifest.json` declares `spec182-case-manifest-v1`
and has no `cases` list, while the driver requires
`spec182-native-case-manifest-v1`. No requester/provider process, namespace,
network request, business oracle, or cleanup result was observed. The raw
`result.json`, exit file, and empty stdout log are retained under
`.codex-tmp/spec184-b5-process-preflight-20260911/`; this is a harness
`UNQUALIFIED` boundary, not a protocol result. See
[`T007 qualification evidence`](../specs/184-native-di-closure/evidence/t007-current-native-qualification-20260911.md).

The first current-candidate MiniNDN owner attempt reached the canonical owner
but passed `--runner-case PO-001` while the runner manifest declared
`PO-001-stream`; it stopped before staging with `case id is not unique`. The
raw attempt is `.codex-tmp/spec184-b5-owner-probe-20260911/`. The corrected
attempt used `PO-001-stream`, ran the current candidate `integration-tests`
binary in the requester namespace, returned exit `0`, and recorded the
business marker plus complete identity/namespace/process-tree/endpoints/
cleanup evidence under `.codex-tmp/spec184-b5-owner-probe-20260911-r2/`.
This closes one bounded owner case only; remaining process/no-Python rows stay
open.

## 2026-09-11 — Spec184 B5 Provider-host lifetime boundary (resolved)

The first unsuppressed B5 ASan/UBSan run stopped with 11,042 bytes in 122 leaked
allocations from `HostState -> ExecutionLeaseService -> makeHostSlotResolver`;
the resolver closure retained the host's target graph through a self-cycle. A
diagnostic all-weak change then stopped at a heap-use-after-free in
`NativeProviderHandlerState::~NativeProviderHandlerState`: the runtime handler
still owns a raw pointer to the host lease table. The final boundary keeps the
host in the Core router and runtime handler, and weakens only the slot resolver.
The rebuilt Provider-host suite passes all eight cases with no sanitizer or
LeakSanitizer report. Raw first-boundary logs remain at
`.codex-tmp/spec184-b5-candidate-asan-20260911/Spec182ProviderHost.log` and
`.codex-tmp/spec184-b5-provider-fix-asan-20260911.log`; the clean rerun is
`.codex-tmp/spec184-b5-provider-fix2-asan-20260911.log`. See
[`B5 component evidence`](../specs/184-native-di-closure/evidence/b5-component-validation-20260911.md).

## 2026-09-11 — Spec184 B5 selector setup boundaries

An initial comma-separated Boost unit filter and two integration selectors
without their parent suite exited 200 during test setup (`no test cases
matching filter`). No product behavior was observed in those attempts. The
correct parent-qualified selectors were then run and passed; the failed logs
remain under `.codex-tmp/spec184-b5-components-20260911/` and
`.codex-tmp/spec184-b5-candidate-integration-focused-20260911/`.

## 2026-09-11 — Spec184 B5 qualification-matrix validator boundary

The first local matrix checker counted the inherited six-column `182:T004` row
as one of the newly added rows and stopped at
`AssertionError: ('182:T004', 6)`. No product code or qualification command ran.
The changed gate limits full-field validation to the new PO/I/FR/CD/INV rows,
retains the failed output at `.codex-tmp/spec184-b5-matrix-validation.log`, and
the rerun reports all expected IDs (14 parent, 16 PO, 8 I, 19 FR, 14 CD, 9 INV)
with `matrix_schema: PASS`. See
[`B5 matrix evidence`](../specs/184-native-di-closure/evidence/b5-matrix-binding-20260911.md).

## 2026-09-11 — Spec184 B4 caller selector runtime boundary

The maintained caller matrix was refreshed against candidate `865e1ee2`. The
current native YOLO ingress, post-selection preparation, provider assembly, and
Qwen stream/conversation selectors passed, and the corrected candidate provider
path made the four Python route/compatibility suites pass 38 tests. The first
Python run used the absent default `build-system-j2/examples/di-native-provider`
path and failed before product execution; the rerun pins
`SPEC181_NATIVE_PROVIDER_BINARY` to the candidate binary.

The older D2b selectors
`ProductionNativeHandlersRunD2bRequestToFinalResponse`,
`ProductionNativeHandlersRunStreamedD2bRequestToFinalResponse`, and
`ProductionNativeHandlersPrepareRolesAfterSelection` reached
`NDNSF_INTEGRATION_BOOTSTRAP_READY` but observed zero response publications and
no role/output records. Their first boundary is the C++ test oracle at
`ndnsf-di-core-flow.t.cpp:4751`, not a protocol result. The raw logs remain under
`.codex-tmp/spec184-b4-cpp-*`; the maintained caller matrix points at the
passing post-selection/assembly selectors and leaves D2b, real-model, no-Python,
and retirement work for Spec184 T006/T007. See
[`B4 evidence`](../specs/184-native-di-closure/evidence/b4-caller-convergence-20260911.md).

## 2026-09-11 — Spec184 B3 checkpoint export and loader provenance boundary

The B3 normal and independent unsuppressed ASan/UBSan C++ selectors passed for
`Spec184NativeCheckpoint/*`: canonical bytes, exact `0600`, symlink refusal, and
pre-rename failure preservation. `DI_NativeRequester` also built and its
`--help` contract passed when the candidate build directory was first in
`LD_LIBRARY_PATH`. A first loader smoke with `/usr/local/lib` ahead of the
candidate selected a stale framework shared library and stopped at an undefined
`DeploymentControlMessage` vtable. This is a library provenance boundary, not a
Spec184 source failure; the successful evidence pins the candidate output first.
Raw outputs and binary identities are recorded in
[`B3 evidence`](../specs/184-native-di-closure/evidence/b3-checkpoint-export-20260911.md).

## 2026-09-11 — Spec184 B2 sanitizer ABI boundary and fixture correction

The first `Spec184DurableOutcome` fixture attempted to observe Provider `FINALIZE` through
retained `waitFor()` records. That harness never entered the intended boundary because
`waitFor()` returns retained records and the handler returns on `COMMIT`; raw attempts remain in
`.codex-tmp/spec184-b2/normal-test-rerun.log`, `normal-test-rerun2.log`, and
`normal-test-rerun3.log`. The changed gate uses the production coordinator's optional
`afterDurableCommit` observation point and C++ atomic release flags. The corrected normal
selector passes (`normal-test-rerun5.log`).

The first strict ASan/UBSan run stopped during fixture teardown with
`AddressSanitizer: new-delete-type-mismatch` while deleting an `ndn::svs::SVSPubSub`; its stack
is at the external NDN-SVS header/library ABI boundary, not a Spec184 production symbol. The
raw report is `.codex-tmp/spec184-b2/asan-test.log`. The dependency was then rebuilt from the
current source/header pair, the sanitizer tree was relinked, and three unsuppressed selector
runs passed with no ASan/UBSan report. The two `new_delete_type_mismatch=0` runs remain diagnostic
only; the clean rebuilt runs are the B2 `DYNAMIC_PASS`. See [`B2 evidence`](../specs/184-native-di-closure/evidence/b2-durable-outcome-20260911.md).

## 2026-09-11 — Spec184 B1 GCC TSan toolchain boundary (resolved with alternate system clang)

The first B1 TSan configure used the normal `/usr/bin/g++ -B/usr/bin` closure and
stopped before compilation because the installed GCC9 package does not provide
`libtsan_preinit.o`; the linker reported `cannot find libtsan_preinit.o`. The
raw configure output is retained at
`.codex-tmp/spec184-b1/tsan/configure.log`. This is a sanitizer-toolchain
availability boundary, not a product or protocol result. The dynamic profile
was then configured in a separate output tree with `/usr/bin/clang++`,
`--toolchain-root=/usr`, and the repository's closed-toolchain checks; the
successful build and six selector repetitions are recorded in
[`B1 evidence`](../specs/184-native-di-closure/evidence/b1-request-correctness-20260911.md).

## 2026-09-11 — Spec184 B1 test fixture namespace compile boundary

The fresh system-first `-j4` B1 build reached the new
`di-native-requester-grant.t.cpp` translation unit after compiling 289/309
tasks, then stopped because the added tests used the nonexistent
`test::NdnsfIntegrationEnvironment` qualifier and an unqualified
`RequestMessage`. No production C++ source failed to compile. The raw build,
configuration, and resource logs remain under `.codex-tmp/spec184-b1/`; the
fixture namespace and type qualification must be corrected before reusing the
partial object set. The follow-up incremental compile accepted the namespace
fix but found the remaining unqualified `ResponseMessage` and the resulting
lambda overload mismatch; that output is `build-retry3.log` in the same raw run
directory and remains a test-only boundary. The next incremental compile
reached the same TU and found that `ResponseMessage::setPayload` requires a
non-const `ndn::Buffer&`; this third test-only boundary is retained in
`build-retry4.log`.

## 2026-09-11 — Spec184 B1 Waf lock/output configuration boundary

The first B1 build attempt configured `build-nac182` successfully with the
system compiler, Boost 1.71, explicit NAC-ABE/SVS/ONNX prefixes, and a fresh
`/tmp/spec184-b1-waf.lock`. The following build used the same environment but
Waf immediately reported `The project was not configured: run "waf configure"
first!`; no C++ task ran. Raw configure/build output and the `vmstat` sample are
retained under `.codex-tmp/spec184-b1/`. This is a Waf lock/output bookkeeping
failure, not a source compile or runtime result. Retry with a fresh lock inside
the configured output tree and preserve this boundary.

## 2026-09-11 — Spec182 R11-B11 replacement-marker stale Waf output boundary

The first `-j3` rebuild after adding the C++ alternate-provider replacement
oracle was invoked with `WAFLOCK=.lock-waf`, but that lock still pointed at the
older `.codex-tmp/spec182-t016-unit-20260911/build` output. The command reached
119/119 and linked a fresh binary there; it did not update `build-nac182`, whose
SHA-256 remained unchanged. No runtime result was taken from this stale-output
attempt. Raw command output and the `vmstat` sample are retained under
`.codex-tmp/spec182-r11-b11-replacement-marker-build-20260911/`. The retry must
use a fresh lock and explicit `build-nac182` configuration before any marker or
qualification claim.

## 2026-09-11 — Spec182 R11-B11 MiniNDN runner selector boundaries

The first current-binary owner probes retained four distinct boundaries. The
conversation selector returned `0` and emitted its native result marker, but
the strace collector ended with one wrapper `exit_group` still unfinished and
`+++ killed by SIGKILL +++`; the runner therefore returned `UNQUALIFIED` with
`TRACE_UNPAIRED`. The alternate-provider selector returned `0` after its
assertions but had no replacement-specific business marker. Both Qwen native
selectors stopped at the test fixture's relative
`tests/fixtures/spec182/qwen-native-config.onnx` lookup because that file was
not staged, and returned `201` before a marker. Raw outputs are retained under
`.codex-tmp/spec182-r11-b11-po001-{conversation,alternate,qwen-stream,qwen-conversation}-202609110007/`.
The retry must preserve the trace-integrity boundary, add only an explicit
replacement oracle, and stage the Qwen fixture with its digest; no failed
selector is counted as a protocol result.

## 2026-09-11 — Spec182 G8 broad selector verbose-log timeout boundary

After the `729555fa` client-close registry checkpoint, the full `*Spec182*/*`
C++ selector was run with `--log_level=test_suite` to retain per-case timing.
The 180-second command timeout expired while executing the longer
`Spec182OnnxWorkerProtocol` frame cases; no failed assertion or protocol error
was emitted before the boundary. Raw output is retained at
`.codex-tmp/spec182-g49-close-registry-20260911/spec182-full.log` with
`spec182-full.rc=124`.

This is a verbose-observer timeout, not a qualification result. The next retry
uses the same fresh binary with `--report_level=no --log_level=message` so the
test process is not slowed by per-case logging; the client suite and targeted
close regression already pass independently.

## 2026-09-11 — Spec182 R11-B9-G8 client-close pending-operation registry compile boundary

The first `-j3` rebuild for the C++ client-close regression reached the final
unit-test translation unit but stopped at the test fixture: `adapters->find()`
returns `shared_ptr<const NativeModelAdapter>`, so the new test attempted a
`dynamic_pointer_cast` that would cast away constness. No production binary or
runtime path was reached. The raw compiler output is retained at
`.codex-tmp/spec182-g49-close-registry-20260911/build-j3.log`.

The fixture now retains the concrete adapter it registers, removing the
constness violation. The retry used the same system-first toolchain and `-j3`,
completed all 190 unit-test build tasks in 57.934 seconds, and showed zero
sustained swap-in/out in the recorded `vmstat` samples. The affected C++ client
suite then passed; this was a test-only first-boundary failure and does not
promote T010/T016 or the remaining native qualification gates.

## 2026-09-11 — Spec182 R11-B8-G48 Qwen native observer type boundary

静态复核发现 Qwen native caller 在 `decode_payload()` 成功返回 JSON array 或 scalar
时仍直接调用 `.get()`。C++ observer 隔离该异常后，terminal 通知可能使 caller 将畸形
事件当作成功。该缺口未触及 Core/Provider；已在
`examples/python/NDNSF-DistributedInference/llm_pipeline/user.py` 增加 mapping 类型门，
并用真实 helper 夹具覆盖 JSON array payload。36 个相关 Python 入口/兼容测试、
`py_compile` 与 `git diff --check` 通过。证据见
[R11-B8-G48](../specs/182-native-di-python-bindings/evidence/r11-b8-g48-qwen-observer-type-guard-20260911.md)。

这是 caller-edge 修复，不是 native request、Provider、跨进程、no-Python 或 T016
资格结果；R11-B8 与 T013/T016/T017 保持原状态。

## 2026-09-11 — Spec182 R11-B8-G47 examples source-closure and `-j3` boundary

The examples-enabled C++ build exposed three target-registration omissions before any
deployment claim was made. The first four-target `-j4` attempt was terminated after available
memory fell to about 0.25 GiB with sustained swap-in/out; this is retained in
`.codex-tmp/spec182-t016-examples-20260911/build.log` and `vmstat-build.log` and is a host
resource boundary. The policy-conforming `-j2` retry completed 277/277 tasks in 31m50.347s,
then the first `di-native-provider-session-smoke` link reported missing canonical ONNX,
`ExecutionLeaseService` and framework publication definitions (`build-j2.log`).

After that closure was repaired, the first `-j3` all-target run reached the
`di-native-plan-schema-smoke` link and reported the same ONNX/framework omission
(`build-all-targets-j3.log`). The next run reached 608/609 and isolated the remaining omission
to `di-native-onnxruntime-smoke` (`build-all-targets-j3-r2.log`). Adding the shared canonical
ONNX source set and candidate framework/ONNX/Protobuf link closure to the three targets made the
bounded retry complete 87/87 in 7m30.258s (`build-onnxruntime-j3-r3.log`). The durable batch
record is [R11-B8-G47](../specs/182-native-di-python-bindings/evidence/r11-b8-g47-examples-source-closure-20260911.md).

The same run also retained a loader identity boundary: with `/usr/local/lib` before the
candidate build root, requester and authority `--help` stopped with old framework undefined
symbols (`DI_NativeRequester-help.log`, `DI_NativeArtifactAuthority-help.log`). Candidate-first
lookup passes `--help`, C++ smoke and Provider `--check-only`; the host build is still rejected
by `verify-runtime-closure.py --reject-prefix /home/tianxing/NDN` as
`RUNTIME_HOST_BOUND_PATH`. Neither condition is counted as a protocol failure, but both are
required gates for the eventual container/multi-machine artifact.

## 2026-09-10 — UAV update slides layout boundary (resolved)

补充模拟到真实 UAV 的差距时，首轮 PDF 构建有纵向溢出；通过缩短文字和调整表格/字号修复。
初始输出 `.codex-tmp/uav-reality-gap/build-1.log` 保留，最终 `build-6.log` 无排版警告，
10 页渲染核对通过。仅文档排版，不是 native/飞行测试结果。
见 [修订与验证记录](NDNSF-UAV/slides/UPDATES_UAV-review.md)。

## 2026-09-11 — Spec182 T016-A broad integration qualification boundary

The fresh current-source `integration-tests --report_level=short` run completed
170 cases with 149 passed, 21 failed, 19 aborted, and 48 failed assertions
(`rc=201`) after 341.73 seconds. The first boundary is
`Spec170NdnsfDiCoreFlow/ProductionNativeHandlersRunD2bRequestToFinalResponse`,
where no response publication or observed role output was produced; the same
run then reports failures across the Spec175 tiny-ONNX and Spec170/175 recovery,
streaming, cancellation, and conversation cases. The complete output is
retained at `.codex-tmp/spec182-t016-unit-20260911/integration-tests-final.log`
and the condensed classification at
`.codex-tmp/spec182-t016-unit-20260911/integration-failure-summary.log`.

The active Spec182 selector was run separately from the same fresh binary and
passed 2/2 cases with 21/21 assertions (`rc=0`); raw output is retained at
`.codex-tmp/spec182-t016-unit-20260911/spec182-integration-selector.log`.
Therefore the active Spec182 integration lane is closed for this validation
checkpoint, while the repository-wide integration lane remains unqualified.
The broad failures are not attributed to the delayed-runner capability fix
without a reproducer at that first boundary, and T016 is not promoted.

## 2026-09-11 — Spec182 T016-A delayed-runner capability boundary

After the fixture callback repair, the fresh current-source full unit selector
reduced the failure to one assertion: `NativeProviderRuntimeCarriesOpaqueStateHandleWithoutHostRoundTrip`
reported `sawCompactState=false`; the run had 1,026 cases with 1 failed case
and 1 failed assertion. The raw output is retained at
`.codex-tmp/spec182-t016-unit-20260911/unit-tests-final.log`. Source inspection
found that `NativeProviderRuntime` checked `supportsOpaqueStateHandles()` before
the delayed preparation callback ran, so a callback-created opaque runner was
misclassified and its state was serialized through host tensors. The fix carries
the resolved runner capability in `ProviderRoleResult` and selects the opaque
state path only after worker execution; no callback is invoked early.

## 2026-09-11 — Spec182 T016-A fixture repair compile boundary

The first incremental build after the bounded fixture repair failed while
compiling `distributed-inference-async-runtime.t.cpp`: two coordinator tests
assigned `config.prepareRunner = [runner] ...` before their local runner had
been declared. The raw compiler output is retained at
`.codex-tmp/spec182-t016-unit-20260911/fixture-repair-build.log`. This was a
test-only patch-context mistake; no production source or runtime was reached.
The repair was narrowed to the exact test functions and must pass a fresh
incremental build before any selector result is counted.

## 2026-09-11 — Spec182 T016-A current-source unit fixture contract

The fresh current-source build completed 309/309 with `-j4`, but its
`unit-tests --report_level=short` run exited `rc=201`: 1,026 cases, 11 failed,
8 aborted, and 20 failed assertions. The first boundary remained
`NativeProviderRuntime requires a runner preparation callback` in
`distributed-inference-async-runtime.t.cpp`; the full output is retained at
`.codex-tmp/spec182-t016-unit-20260911/unit-tests.log`. Because the binary was
rebuilt from the current `HEAD`, this is a current C++ fixture/contract failure,
not a stale-artifact result. No T016 status is promoted. The next gate is source
analysis of every `runSamplingEpochs` call and its runner-preparation callback,
followed by a bounded fixture repair and fresh C++ selectors.

## 2026-09-11 — Spec182 T016-A target configuration boundary

The fresh current-source configure completed successfully, but the first Waf
build requested example targets while the configuration had not enabled
examples. Waf stopped before compiling with `Could not find a task generator for
the name 'DI_NativeRequester'`; the complete command/output is retained in
`.codex-tmp/spec182-t016-unit-20260911/build.log`. This is a target-selection
configuration boundary, not a C++ compile or runtime result. The retry uses the
configured `unit-tests` and `integration-tests` targets, then a separate
`--with-examples` configuration for the requester/Provider closure.

## 2026-09-11 — Spec182 T016-A stale unit binary boundary

The first full C++ unit qualification attempt used the existing
`build-nac182/unit-tests` binary. It exited `rc=201` after about 330 seconds
with 11 failed cases and 8 aborted cases; the first failures were
`NativeProviderRuntime requires a runner preparation callback` in conversation
and opaque-state tests. The raw run is retained at
`.codex-tmp/spec182-t016-unit-20260911/`. The binary predated the current
fixture repair even though the source tree had no C++ diff after the native
checkpoint, so this result is a stale-artifact boundary rather than a current
protocol result. The changed gate is a fresh current-source build in a new
output directory before any T016 status decision; no task is promoted by the
failed run.

## 2026-09-10 — Spec182 R11-B8-G40 network integration retry boundary

The first normal run of the G40 network integration exited 1 after the injected
pre-start cases, with only a `BrokenPipeError` from the fake `srun` output
fixture. No NFD or application process was started and no product assertion was
reported. The complete xtrace is retained at
`.codex-tmp/spec182-r11-b8-g40-network-failure-20260910/network-xtrace.log`.
A bounded xtrace rerun and a fresh normal rerun both reached
`NETWORK_SCRIPT_PASS`; the result is recorded in
[R11-B8-G40 evidence](../specs/182-native-di-python-bindings/evidence/r11-b8-g40-scratch-symlink-boundary-20260910.md).
The transient fixture retry is not counted as a product failure or qualification result.

## 2026-09-10 — Proposal DNMP reference download format boundary

The direct download of the DNMP reference returned HTML rather than PDF;
`pdftotext` therefore rejected the document format. The received bytes are
preserved at `.codex-tmp/proposal-dnmp-example-20260910/dnmp.pdf` (HTML despite
the suffix). The browser's indexed primary-paper text was used instead,
including Sections 2.4, 3.2 and 3.3. No product build or protocol test failed.
The source and claim corrections are recorded in
[the proposal audit](PAPER/proposal-defense/dnmp-comparison-review-20260910.md).

## 2026-09-10 — Spec182 R11-B8-G12 host identity selector compile boundary

The first fresh `unit-tests` build for the Provider identity binding batch
rejected the test-only alias `constexpr char[] = HOST_PROVIDER_NAME` because
the compiler cannot infer an array size from another array. The production
change was not reached as a runtime failure; the raw compiler output is
retained at `.codex-tmp/spec182-r11-b8-g12-provider-identity-20260910/build.log`.
The alias was changed to a `const char*` with the same literal contents before
the bounded retry. No test result from this failed build is counted as PASS.

## 2026-09-10 — Spec182 R11-B8 prepared-role fixture checker boundary

The bounded C++ fixture repair first ran cppcheck against
`tests/unit-tests/distributed-inference-async-runtime.t.cpp`. cppcheck stopped
with its internal `AST broken: endless recursion from 'config'`
`internalAstError` at the existing aggregate configuration construction. This
is a checker/parser boundary, not a compiler or production-runtime failure.
The changed gate was therefore the system-first fresh `unit-tests` build plus
the C++ `Spec182EpochText`, `Spec182StreamAcceptance`, and
`Spec182GenerationOptions` selectors. The build completed 190/190 steps and all
three selectors exited 0; durable details are in
[R11-B8 fixture evidence](../specs/182-native-di-python-bindings/evidence/r11-b8-cpp-fixture-20260910.md).
The broad `Spec182*` selector remains outside this bounded result.

The follow-up fresh complete C++ `Spec182*` selector then passed 256 cases and
7077 assertions with exit 0 after the fixture repair. This resolves the recorded
runner-callback fixture boundary for the unit suite; it does not change the
maintained-caller, no-Python, dependency-closure, or T016/T017 status.

## 2026-09-10 — Spec182 R11-B1-PY native binding dependency and ownership boundaries

The first Python extension rebuild was invoked from the wrong working directory and
stopped before configuration (`pythonWrapper/build-nac182` did not exist); the raw
diagnostic is `.codex-tmp/spec182-r11-b8-authority/extension-build.log`. The next
attempt used the host `/usr/local` NAC-ABE prefix, but `ldd -r` exposed the first
native boundary as unresolved `ndn::nacabe::Consumer::clearCache` and related symbols
because that library did not match the current `build-nac182` framework; the raw
diagnostic is `.codex-tmp/spec182-r11-b8-authority/extension-build-2.log`. A subsequent
rebuild against the stale DI library had the same class of unresolved native symbols
(`extension-build-3.log`). The native DI library was rebuilt with the matching
`/home/tianxing/NDN/nac-abe-integration-182/install` dependency, then the extension was
rebuilt and imported successfully (`extension-build-4.log`). These attempts are
dependency/build-boundary failures, not product behavior results; the corrected
ownership boundary and validation are recorded in
[R11-B1-PY evidence](../specs/182-native-di-python-bindings/evidence/r11-b1-py-native-authority-20260910.md).

## 2026-09-10 — Spec182 R11-B6 native replacement validation boundaries

The first replacement harness attempt could not find Provider A's public key because the
encoded PIB key names were compared as raw bytes. The driver stopped with `StopIteration` while
decoding the Name blobs; the raw attempt is retained under
`/tmp/spec182-r11-b6-replacement-1789043701/`. The repair decodes the PIB Name and selects the
candidate URI, then reruns with independent A/B identities.

The next harness attempt reached Provider B's real `attempt-2` execution but asserted a text
form that did not match the JSON log (`"attemptEpoch":"2"` and `attempt-2`). The raw diagnostic
is `.codex-tmp/spec182-r11-b6-build/replacement-2.log`; the assertion now accepts the actual
attempt marker and still requires B grant verification, CPU ONNX execution, and the stream oracle.

The no-backup branch initially expected `NATIVE_STREAM_FAILED`, but the native requester
correctly reports its first boundary as
`NATIVE_REQUEST_STAGE_FAILED boundary=ACK_CLOSED ... DI_NATIVE_NO_ADMITTED_PROVIDER`. The
expected marker was corrected in the driver; raw output is
`.codex-tmp/spec182-r11-b6-build/no-backup.log` and the checked negative run is recorded in
[R11-B6 evidence](../specs/182-native-di-python-bindings/evidence/r11-b6-native-replacement-20260910.md).

The broad C++ `Spec182*` selector remains red in six existing sampling/epoch-text fixtures. All
six throw `std::invalid_argument: NativeProviderRuntime requires a runner preparation callback`
before the relevant assertions; raw output is `.codex-tmp/spec182-r11-b6-build/unit-selector.log`.
The focused `Spec182StreamAcceptance` selector and the
`Spec170NdnsfDiCoreFlow/Spec182*` integration selector pass, so this is preserved as a fixture
boundary rather than attributed to native replacement.

## 2026-09-10 — Spec182 R11-B5 Provider restart recovery boundary

After the first C++ `FULL_CONTEXT` turn persisted the requester checkpoint, the Provider was
terminated with SIGKILL and restarted from the same configuration. The restarted Provider had no
durable KV state and rejected the resumed role with `PROVIDER_CONVERSATION_STATE_MISSING`; the
requester returned `NATIVE_STREAM_FAILED` without a second success/checkpoint. The restart log
contained no execution-evidence or stream-event marker, so no duplicate prefix was executed or
published. The first harness attempt exposed an overly narrow expected error marker and was
repaired before the checked run. Raw output and the durable result are recorded in
[R11-B5 evidence](../specs/182-native-di-python-bindings/evidence/r11-b5-native-recovery-20260910.md).
This is the designed safe-rejection boundary, not proof of durable Provider KV recovery or whole
Spec qualification.

## 2026-09-10 — Spec182 R11-B4 repeated-request artifact identity boundary

The fresh-generation retry completed the first native continuation turn and accepted the
second turn's new generation identity, then Provider assembly stopped at
`DI_CANONICAL_ROOT_DIGEST_MISMATCH`. The requester publisher reused one stable
`assignedArtifact` name across requests while each canonical root carried a request-scoped
source publication name; Provider's process-wide artifact cache consequently returned the
first root for the second request. Raw output is retained under
`/tmp/spec182-r11-b4-fresh-generation/`. The C++ publisher now includes the canonical
manifest digest in the stable identity. A fresh rebuild and rerun completed both
`FULL_CONTEXT` and new-generation `APPEND_DELTA` under `/tmp/spec182-r11-b4-final-checked/`; the bounded resolution is recorded in
[R11-B4 evidence](../specs/182-native-di-python-bindings/evidence/r11-b4-native-continuation-20260910.md).
This was a native cross-request binding boundary, not a Python wrapper or qualification result.

## 2026-09-10 — Spec182 R11-B4 continuation fresh-state identity boundary

The Provider control replay repair allowed the first C++ `FULL_CONTEXT` turn to complete,
persist its native checkpoint, and emit the stream oracle/success markers. The second
process then failed at the native coordinator contract
`native conversation continuation requires a fresh stateful request` because the fixture
reused the first turn's `generationId`. The raw run is retained under
`/tmp/spec182-r11-b4-ackfix/`. The retry must create a distinct second request/generation
identity while retaining the committed parent checkpoint; this is a native harness
contract boundary, not a Python binding or qualification result.

## 2026-09-10 — Spec182 R11-B4 conversation COMMIT acknowledgement boundary

The traced C++ continuation process received stream cursors `1..8` and terminal cursor
`9`, entered the conversation commit phase, and showed the Provider decrypting the valid
requester `COMMIT` control. The requester then failed with
`NATIVE_CONVERSATION_COMMIT_ACK_INCOMPLETE`. Provider output contains a rapidly growing
sequence of duplicate `/ndnsf-di/conversation/commit` publications because its polling
loop reprocessed historical controls. Raw output is retained under
`/tmp/spec182-r11-b4-trace/`. The next repair is a C++ per-sequence control replay guard,
followed by a fresh native rebuild and the same two-round continuation/wrong-parent run.
This remains an integration boundary and is not a Python binding or qualification result.

## 2026-09-10 — Spec182 R11-B4 continuation completion boundary

The terminal-role checkpoint finalization repair executed the additional state-only ONNX
Runtime epoch and the Provider emitted stream cursors `1..8` plus cursor `9`. The requester
accepted the encrypted Response but stayed pending until its 30-second request budget expired;
no checkpoint or success marker was written. Raw output is retained under
`/tmp/spec182-r11-b4-finalize/`. The next retry must distinguish a missing terminal stream
cursor from a missing conversation COMMIT acknowledgement, using C++ trace only. This remains
an integration boundary and is not a Python binding or qualification result.

## 2026-09-10 — Spec182 R11-B4 continuation lineage boundary

After the deferred decode-state promotion repair, a fresh independent C++ continuation
probe reached real Provider ONNX execution and published an authenticated receipt. The
requester then stopped at `conversation receipt lineage mismatch`: the Provider identity
included input prefix token `[3]`, while the fixture's FULL_CONTEXT canonical prefix was
empty. The raw run is retained under `/tmp/spec182-r11-b3-probe-vyoahjfs/`. The fixture is
being corrected to declare the exact input prefix before the next retry. This remains a
development boundary and is not a Python binding or qualification result.

## 2026-09-10 — Spec182 R11-B4 continuation prefix-count boundary

The corrected FULL_CONTEXT prefix `[3]` allowed the independent C++ process to reach the
receipt scope checks, but the requester rejected `prefixTokenCount`: Provider receipt and
checkpoint values differ despite the prefix digest being accepted. Raw output is retained
under `/tmp/spec182-r11-b4-probe-current/`. A temporary diagnostic will expose the exact
wire values before the native count contract is repaired. This is still a C++ continuation
boundary, not a Python binding or qualification result.

## 2026-09-10 — Spec182 R11-B4 deferred decode-state promotion boundary

The first independent C++ continuation probe reached real Provider ONNX execution, then
failed at `PROVIDER_CONVERSATION_PROMOTION_STAGE_FAILED`. The streamed requester observed
the resulting gap timeout and no conversation journal checkpoint was committed. The first
boundary was a C++ ownership mismatch: conversation turns defer decode-state commit, while
the CPU promotion branch only searched committed state; candidate-only cleanup also did not
match the existing erase key. Raw output is retained under
`/tmp/spec182-r11-b3-probe-9fwp2_mg/`. The retry adds exact candidate lookup and candidate-aware
cleanup, then must rebuild and rerun the C++ two-round continuation plus wrong-parent negative.
This is not evidence against the Python binding, and no qualification pass is claimed.

## 2026-09-10 — Proposal local checkpoint hook boundary

The document checkpoint was rejected by the pre-commit hook's full-index assistant
reference scan. The hook and previous records explicitly support
`NDNSF_LOCAL_CHECKPOINT=1` for local checkpoints; retry uses that entry and retains
the prohibited-path check. No hook is changed and no remote operation is requested.
Diagnostic: `.codex-tmp/proposal-two-designs-20260910/checkpoint-hook.log`.
Scope and checks: [proposal audit](PAPER/proposal-defense/research-revision-audit.md).

## 2026-09-10 — Proposal PPTX relative-path resolution boundary

The two-design authorization revision compiled successfully, but PPTX conversion
stopped at `pdftohtml`: the converter resolves input paths relative to its own
slides directory, so a repository-relative `--pdf` duplicated that directory.
No PPTX was generated. The retry uses absolute PDF and notes paths and a fresh
private build directory; no product runtime was involved. Raw failure:
`.codex-tmp/proposal-two-designs-20260910/pptx-build.log`.
Durable follow-up: [proposal audit](PAPER/proposal-defense/research-revision-audit.md).
Resolved with absolute input paths: `pptx-retry.log` records 791/791 source spans
assigned once and 38 notes pages; LibreOffice re-export and rendered review passed.

## 2026-09-10 — Spec182 R10-B84 native request-scope wire compatibility boundary

After adding a fresh per-client owner scope to the production C++ request identity, the
shared-library `Spec170NdnsfDiCoreFlow/Spec182*` selector rebuilt successfully but exited
`201`: all nine C++ cases received an ACK and then stopped before collaboration (`collaborationCalls=0`),
with stream gaps, unary timeouts, or `DI_NATIVE_NO_ADMITTED_PROVIDER`. The raw output is retained at
`.codex-tmp/spec182-r10-b84b-request-id-integration.log`. The C++ unit identity selector still passed,
so this is a native Core/Provider name-contract regression, not evidence of Python binding failure or
qualification. The next retry must preserve the failure, locate the first parser/filter boundary, and
prove the repaired request identity through the same C++ integration selector.

## 2026-09-10 — Spec182 R10-B83 integration target selection boundary

The first post-loader integration rebuild used the current Waf cache's default output tree and
stopped immediately with `Could not find a task generator for the name 'integration-tests'`
(exit `1`). No compiler or test task ran; the selected cache was the old
`.codex-tmp/spec182-r10-b78-provider/build` tree. Re-running with the explicit current output
directory `.codex-tmp/spec182-r4-b2/build` rebuilt all 118 integration tasks successfully, and
the native `Spec170NdnsfDiCoreFlow/Spec182*` selection passed 9 C++ cases with no errors. The raw
failed command and successful retry are retained under
`.codex-tmp/spec182-r10-b83-conversation-loader/`; this was a Waf output/cache boundary, not a
native protocol failure.

## 2026-09-09 — Proposal rendering dependency boundary

The two-reason authorization revision compiled successfully, but the first rendered
review stopped at `import fitz` (`ModuleNotFoundError`) before opening any PDF.
A temporary pip environment could not resolve its package host; that install was
stopped. The previous document run retained CPython 3.8 packages under
`.codex-tmp/proposal-research-revision-20260909/pydeps`. A Python 3.10 import probe
failed on the incompatible lxml extension; system Python 3.8 plus a command-local
PYTHONPATH imported all document dependencies and completed rendering and PPTX generation.
This was a document-tool prerequisite, not a native build or protocol failure.
See [revision audit](PAPER/proposal-defense/research-revision-audit.md) and
`.codex-tmp/proposal-two-reasons-20260909/render-dependencies.log`, `render.log`,
and `pptx-build.log`. No product dependency or qualification status was changed.

## 2026-09-09 — Spec182 R10-B72 Provider plan service-selection boundary

The first metadata-only Provider `--check-only` probe used the executable default service
`/AI/YOLO/2x2Inference` while the reused four-role bundle plan declares
`/Inference/NativeTracer`; the parser stopped before role registration with
`native execution plan has no service` (exit `2`). The raw output is retained under
`.codex-tmp/spec182-r10-b72-provider-check-20260909/check-only.log`. Supplying the declared
service name reached `NDNSF_DI_NATIVE_PROVIDER_PLAN_READY` and
`NDNSF_DI_NATIVE_PROVIDER_CHECK_OK` (exit `0`). This is a command/configuration boundary,
not a Provider protocol or qualification result; see
[R10-B72 evidence](../specs/182-native-di-python-bindings/evidence/r10-b72-provider-plan-check-20260909.md).

## 2026-09-09 — Spec182 R10-B70 local checkpoint hook boundary

The first checkpoint commit attempt for the R10-B68 evidence was rejected before commit by
the repository pre-commit hook's full-index development-assistant text scan. The reported
matches were pre-existing `.specify/memory` references to `AGENTS/CLAUDE`; no R10-B68 file or
product source was implicated. The hook explicitly supports `NDNSF_LOCAL_CHECKPOINT=1` for
local checkpoints, so the same three explicit Spec paths were staged and committed with that
flag. This is a repository hook boundary, not a product, protocol, or qualification result;
the raw terminal output remains in the session and the retry checkpoint is
`13d0c1b5`.

## 2026-09-09 — Spec182 R10-B67 Python extension loader boundary

The first post-mapping extension test invocation loaded `/usr/local/lib/libnac-abe.so`
through the candidate build tree and stopped before any Python test body with the missing
`ndn::nacabe::Consumer::clearCache` symbol (exit `1`). Re-running with the explicit
`/home/tianxing/NDN/nac-abe-integration-182/install/lib` runtime prefix reached the candidate
extension, then stopped before test execution because the stale
`.codex-tmp/spec182-r4-b2/build/libndnsf-distributed-inference.so` did not yet export
`NativeInferenceHandle::applicationRequestId()` (exit `1`). No native request, Core packet,
or Qwen behavior was observed. Raw output and return code are retained under
`.codex-tmp/spec182-r10-b67-binding-boundary-20260909/`.

The next retry gate is to rebuild the candidate `ndnsf-distributed-inference` shared library
from the same source checkpoint as `unit-tests`, then run `ldd`/`nm` with the explicit NAC-ABE
prefix before interpreting Python binding results. This is a dependency/source-closure issue,
not a protocol result.

## 2026-09-09 — Spec182 R10-B47 PO-001 owner/runner retry boundaries and pass

The first root-enabled retry of the T016 owner reached MiniNDN setup but failed before a
business process because the command-local system-first `PATH` omitted `/usr/local/bin/infoconv`;
the owner recorded `MININDN_OWNER_FAILED:JSONDecodeError` in
`.codex-tmp/spec182-t016-r10-b46-owner5`'s predecessor run. Adding the installed runtime tool
path exposed the next runner contract boundary: the historical PO-001 manifest used process role
`native-di-integration`, which is outside the runner's declared role vocabulary. A corrected
transient manifest bound the same process to role `requester`, then failed its artifact digest
preflight because the current incremental `integration-tests` binary had changed.

Those failures are preserved in the fresh run directories
`.codex-tmp/spec182-t016-r10-b46-owner/`, `owner3/`, and `owner4/`; none was classified as a
protocol result. After recomputing only the executable artifact hash and retaining the explicit
role binding, `sudo -n env PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin python3
Experiments/NDNSF_DI_NativeClosure_Minindn.py ... --execute-owner` completed PO-001 in
`.codex-tmp/spec182-t016-r10-b46-owner5/`: owner namespaces and NFD sockets were live, the
staged native process returned `0` in `8001ms`, the business marker
`SPEC182_NATIVE_DI_REQUEST_RESULT_OK` was present, and the collector reported complete identity,
process-tree, namespace, exec-map, endpoint, business-oracle and cleanup evidence with no
integrity/policy violations.

This is a real isolated native-process PO-001 observation for the existing in-process
requester/Provider fixture. It does not prove independent requester/Provider transport, I02-I08,
PO-002-PO-014, maintained caller/no-Python migration or full T016 qualification; those remain
open. See [R10-B47 evidence](../specs/182-native-di-python-bindings/evidence/r10-b47-t016-po001-owner-pass-20260909.md).

## 2026-09-09 — Spec182 R10-B26 missing `REPO_REF` negative recheck

The old R10-B6 missing-object failure was a test-boundary observation: the fixture's fixed
three-second pump ended before the production fetch exhausted its default 30-second budget. The
current source already scopes `NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS=1000` with RAII for this
negative case. A fresh run of
`.codex-tmp/spec182-r4-b2/build/integration-tests --run_test=Spec170NativePostSelection/ProductionIngressRejectsMissingNativeRepositoryReference`
entered the real Provider handler and exited `0` in 1.946850 seconds with the expected fetch
failure, no runner input, no successful Response, and no test errors. The raw retry is retained
under `.codex-tmp/spec182-r10-b26-missing-repo-ref-20260909/`.

This closes the local missing-object negative recheck while preserving the original R10-B6
failure as history. Cross-process transport, maintained callers and T016 qualification remain
unobserved.

## 2026-09-09 — Spec182 R10-B18 runner dynamic-ELF and trace-integrity boundaries

Three exploratory runner probes exposed sequential harness boundaries before the final retry:

- `.codex-tmp/spec182-runner-probe-20260909051337/` exited before bwrap because the minimal
  runner environment had no `PATH` and the manifest used relative `bwrap`/`strace` names.
- `.codex-tmp/spec182-runner-probe2-20260909051429/` reached bwrap but dynamic `/bin/true`
  failed `execve` with `ENOENT`; loader and libc were staged only below `/probe-root`, while ELF
  absolute interpreter/DT_NEEDED paths were not mounted.
- `.codex-tmp/spec182-runner-probe3-20260909051640/` executed `/bin/true` with return code 0,
  but the collector marked normal strace `<unfinished ...>`/`<... resumed>` pairs as
  `TRACE_UNPAIRED`, producing `UNQUALIFIED` before evaluation.

These are runner tool/observation boundaries, not DI protocol results. The retained retry
`.codex-tmp/spec182-runner-probe4-20260909051727/` passed dynamic execution (`returncode=0`) and
reported a complete trace after absolute shared-library mounts and per-PID unfinished/resumed
pairing were repaired. The result remains `UNQUALIFIED` because this probe intentionally lacks
business evidence.

## 2026-09-09 — Spec182 R10-B17 owner invocation preflight boundary

The first explicit R10-B17 owner invocation used the frozen registration manifest without a
`campaignCase` selector. The owner rejected it with exit `2` / `campaignCase is required` before
MiniNDN, NFD, or any namespace was started. This is an operator invocation boundary, not a
protocol or qualification result.

Raw stdout/stderr are retained as `.codex-tmp/spec182-t016-r10-b17-20260909050906.stdout` and
`.codex-tmp/spec182-t016-r10-b17-20260909050906.stderr`; the output directory contains only the
explicit `UNQUALIFIED` result. The retry must use a fresh manifest copy with `campaignCase=I01`.

## 2026-09-09 — Spec182 T016 MiniNDN owner preflight recheck

A fresh T016 campaign preflight was run after starting the local NFD. The socket check and
`nfdc status report` both succeeded (`/run/nfd/nfd.sock`, NFD 24.07-14-g2b43d675), but
`Experiments/NDNSF_DI_NativeClosure_Minindn.py` still exited `2` before starting any business
process or namespace run with `MININDN_NODE_CONTEXT_NOT_PROVIDED` for `campaignCase=I01`.
`ip netns list` contained no campaign namespace, and the owner supplied no node/netns metadata
for `tests/standalone/run-spec182-native-closure.py`; therefore this is an owner-preflight
boundary, not a protocol, requester, Provider, or qualification result.

The complete raw retry is retained under `.codex-tmp/spec182-t016-r5/`, with the NFD startup
record under `.codex-tmp/spec182-t016-preflight-20260909/`. NFD availability changes the prior
socket blocker, but does not close the missing MiniNDN node-context requirement. T016 remains
`UNQUALIFIED` and must be retried only after a real isolated node/netns/socket context is created.

## 2026-09-09 — Spec182 full integration stale recipe-oracle boundary

The first full `integration-tests --log_level=test_suite` run after the R10-B11 checkpoint
completed the suite but exited `201` with four failures in
`Spec175NativeAssembly/{AssignmentBoundRootSourceAndCachePath,RegisteredOneProviderAssemblyLoadsOrt,RegisteredTwoProviderAssemblyLoadsOrt,RegisteredFourProviderAssemblyLoadsOrt}`.
Each failed before ORT execution with `DI_NATIVE_ONNX_RECIPE`: the integration fixture's
`recipeDigestFor` helper sorted input/output names while the production
`canonicalNativeOnnxRecipeJson` binds those names to contract order. This is a stale test
oracle boundary, not a protocol result. The raw complete run and vmstat are retained under
`.codex-tmp/spec182-t016-r3/`; the unit suite in the same checkpoint passed.

The helper was corrected to preserve contract order and the stale graph digest/adapter identity
in the same fixture was aligned with the current source identity. A system-first `-j2` rebuild
completed successfully and the affected `Spec175NativeAssembly/*` suite passed 7/7; the complete
integration suite is still a separate retry. The lower concurrency was selected because the full
unit/integration attempts produced sustained swap-in on this host.

## 2026-09-09 — Spec182 R10-B9 requester REPO_REF oracle boundary

The first R10-B9 repository-reference selector failed in its test oracle: the
observer built a `std::string` from `begin()` and `end()` iterators obtained from
two temporary `RequestMessage` payload buffers.  That undefined test behavior
first appeared as a missing observation and then as an empty-JSON planning
error, even though the request wire had been produced.  The raw attempts are
retained under `.codex-tmp/spec182-r10-b9/` (`selector-r2.log` and
`selector-r3.log`).  The oracle now copies one payload buffer before inspecting
it; the existing inline selector and the new `REPO_REF` selector both pass.

A separate comma-separated Boost.Test filter attempt returned the setup error
“no test cases matching filter”; it was a selector syntax boundary, not a
product or protocol result.  The final evidence uses separate selector
commands and records the corrected test-harness boundary.

## 2026-09-09 — Spec182 R10-B2 facade test-double boundary

The first R10-B2 focused Python run failed in the new native reference facade
test because its fake `NativeApplicationInput` did not model the C++ class's
default empty `payload` and `options` fields. This was a test-harness boundary,
not a native or protocol failure. The fake was corrected, the same suite was
rerun with 17 passing cases, and the raw retry output is retained at
`.codex-tmp/spec182-r10-b2/app_sdk.log`; the batch evidence records the first
failure boundary and the changed test-double check.

## 2026-09-09 — Proposal shared bibliography build boundary

The first EN/CH research-revision build reached BibTeX but failed to resolve
`./research-references.bib` from the isolated output directory. No protocol or
experiment was run. The source now uses an unqualified bibliography basename;
the alternate language build directories link to the same shared bibliography.
Initial logs are retained at `.codex-tmp/proposal-research-revision-20260909/en-build.log`
and `ch-build.log`; later attempts use separately named logs. This document-only
boundary is tracked in [the revision audit](PAPER/proposal-defense/research-revision-audit.md).

The EN/CH second build passed. A later multi-hunk wording patch was rejected
because a paragraph fragment was supplied as a whole-line match; it changed
nothing and was reapplied with the exact complete table row. A broad read-only
result-file discovery also hit protected runtime-state directories; no permission
changes were made. The audit uses named evidence paths, and marks the older DI
raw-run provenance as not re-established rather than assuming a result from that search.

The initial `review_rendered.py` invocation failed at import because the default
Python lacked PyMuPDF (`fitz`). This is a rendering-tool environment boundary;
the PDF build had succeeded. The dependency-verified interpreter is used for
rendering and PPTX generation; no research result is inferred from this check.

The first PPTX export was stopped by the generator's approved-build-root guard
before conversion. The supplied `.codex-tmp/.../ndnsf-pptx-build` path was not
an allowed reset target; no output was cleared. Retry uses a newly allocated
`/tmp/ndnsf-*` build directory. The first visual review also caught an obsolete
three-seed chart selected for a ten-seed caption; it is replaced with a table
derived from the exact ten-seed aggregates before delivery.

The second PPTX invocation reached the next safety guard: an existing `mktemp`
directory lacks the generator's ownership marker. It was not cleared. The next
invocation uses a nonexistent `ndnsf-build` child inside that dedicated temporary
directory, allowing the generator to establish its own marker safely.

The third PPTX attempt completed conversion but failed before publication because
the existing notes parser did not accept generated one-line `slideentry` blocks.
The generated notes are adjusted to the parser's documented multiline form;
the protected staged output was not published as a completed deck.

Presenter-notes PDF compilation then exposed PDF-extracted Unicode ligatures and
the mathematical ell character that pdfLaTeX did not accept. The generator now
normalizes ligatures and spells out ell in notes. This affects derived presenter
text only, not the source slide formula or experimental values. Original failure
log: `.codex-tmp/proposal-research-revision-20260909/notes-build.log`.

## 2026-09-09 — Official defense criteria document extraction

Web DOCX parsing was unsupported; urllib plus ZIP/XML extraction succeeded.
A supplementary HTML extraction failed before fetching because `bs4` was absent.
The standard-library HTMLParser fallback succeeded without installing dependencies.
Graduate Catalog retrieval still produced no valid requirements text (web 403);
the report explicitly leaves the complete catalog rules unverified.
A progress-record patch with an unmatched failure-log context was rejected without
changes; after reading the exact lines, the corrected patch was applied separately.
This is a document-tool boundary, not research or protocol evidence. See
`.codex-tmp/proposal-reviewed-20260909/official-criteria/extraction-boundary.md` and
[source-based review](PAPER/proposal-defense/review.md).

## 2026-09-08 — Spec182 R7-B1 replacement expectation boundary

The first `Spec182R4B6RealProviderConversationReplacement` run built successfully but exited
201 at the assertion that a one-Provider fixture would complete a successful replacement. Its
first runtime boundary was the recovery `ACK_CLOSED` planning step:
`NATIVE_REQUEST_STAGE_FAILED` with cause `DI_NATIVE_NO_ADMITTED_PROVIDER`; the failed Provider
was excluded from the recovery admission set, so no alternate Provider could be selected.
This was a test expectation boundary, not permission to retry the same Provider or publish a
partial checkpoint. The raw failure is retained at
`.codex-tmp/spec182-r7-b1-r4b6-replacement-20260908/replacement.log` with
`replacement.rc=201`.

The test was corrected to assert the contract's bounded single-Provider negative: one Provider
collaboration call, two ACK calls, `NATIVE_REQUEST_STAGE_FAILED`/`ACK_CLOSED`, and no conversation
checkpoint. The final selector passed. A successful alternate-Provider recovery still needs a
separate multi-Provider harness and remains open under T011-C/T016. See
[R7-B1 evidence](../specs/182-native-di-python-bindings/evidence/r7-b1-r4b6-replacement-20260908.md).

## Proposal authorization documentation patch validation

首次 apply_patch 因同一 patch 重复声明 main.tex 被结构验证拒绝，未写入目标文件。
将同一文件的修改合并为一个操作后重试；不是构建、测试或协议失败。
见 [revision evidence](PAPER/proposal-defense/authorization-revision.md#editing-boundary)。

正文首次 latexmk exit0 但新增文献引用仍 undefined，不能视为通过；
重试使用独立辅助目录和禁用旧 latexmk 配置，日志及原因见上述 revision evidence。

## 2026-09-08 — Authorization comparison documentation checkpoint

论文比较文档首次 `git add` 被既有 `docs/*` ignore 规则拒绝，随后仅对用户要求的
`authorization-design-comparison.md` 使用显式 `git add -f`。首次 commit 被 pre-commit
扫描全索引中的历史助手引用拒绝；边界为提交钩子，不是文档检查失败。
核对 `.git/hooks/pre-commit` 后采用既有 `NDNSF_LOCAL_CHECKPOINT=1` 本地模式，
仍执行禁止路径检查。[材料与核对记录](PAPER/named-data-network-service-framework-paper/authorization-design-comparison.md#review-and-validation-boundary)。

## 2026-09-08 — Spec182 R5-B7 placement selector transient SIGSEGV

R5-B7 首次运行完整 `Spec182V3Placement/*` 选择器时，测试进程在
`PublicClientCommitsSignedOfferAndIgnoresLateTerminalCallbacks`、
`tests/unit-tests/di-native-v3-placement.t.cpp:1026` 处发生 SIGSEGV，约 0.85s
后 exit201。首个边界是测试进程内存访问错误；没有进入可据此判断的 Provider、协议或
qualification 结果。原始输出保留在
[r0 placement log](../.codex-tmp/spec182-r5-b7-placement-suite-20260908-r0/placement.log)。

随后隔离该 selector（r1）和再次运行完整 9-case selector（r2）均 exit0、无错误；记录见
`.codex-tmp/spec182-r5-b7-placement-suite-20260908-r1/` 与
`.codex-tmp/spec182-r5-b7-placement-suite-20260908-r2/`。因此本次按瞬时测试边界保留，
不将重试通过升级为真实 Provider 或跨进程资格结论；若再次出现，需先比较新的原始运行
目录和首个失败边界，再决定是否修复测试或产品代码。

## 2026-09-08 — Spec182 R4-B5 public conversation build invocation

首次重建命令将 `PATH=...` 置于 `/usr/bin/time` 后，`time` 把它当作待执行程序，
在编译器启动前返回 `exit127`。原始 [r2日志](../.codex-tmp/spec182-r4-b5-public/build-r2.log)
保留；这是执行器命令语法边界，不是源码编译或测试结果。重试使用 command-local
PATH 并保持 `-j4`。

## 2026-09-08 — Spec182 integration source closure and grant publication freshness

集成目标首次重新链接在源文件编译完成后 exit1：`tests/wscript` 的手工
`di_integration_sources` 漏掉会话、请求准备、request envelope、catalog 和授权等当前
DI core TU，首个边界是链接器 undefined reference，不是协议或 Provider 运行结果。
原始日志见 [build-integration-r2.log](../.codex-tmp/spec182-r4-b4-current/build-integration-r2.log)。
改为与生产库相同的 DI core/adapters glob 后，`integration-tests -j4` exit0（52.565s）。

随后 `Spec182GrantClientFlow/*` 首次运行 exit201：Core 发布器传给
`publishSignedAppData` 的 ndn-cxx 默认 freshness 为 0，被已有正 freshness 门拒绝；首个
运行边界是 `signed APP Data freshness must be positive`，不是签名或解包失败。原始日志见
[integration-spec182-grant.log](../.codex-tmp/spec182-r4-b4-current/integration-spec182-grant.log)。
固定授权发布 freshness 为 60000ms 后，DI 集成目标增量构建 exit0（20.599s），
`Spec182GrantClientFlow/*` 2 cases exit0；结果见
[integration-spec182-grant-r2.log](../.codex-tmp/spec182-r4-b4-current/integration-spec182-grant-r2.log)。
随后加入正 freshness 回归断言，增量构建再次 exit0（20.546s），同一 2 cases 再次 exit0，
详见 [integration-spec182-grant-r3.log](../.codex-tmp/spec182-r4-b4-current/integration-spec182-grant-r3.log)。
当前集成目标没有公开两轮会话选择器，因此 T011-C/T016 仍保持 `PARTIAL`。

## 2026-09-08 — Spec182 R4-B4 conversation Unicode canonical boundary

首次R4-B4增量构建使用旧`-j2`并成功完成；随后`Spec182Conversation*`运行9 cases，
8项失败。首个代码边界是`NativeCanonicalJson`使用`ensure_ascii=true`，与旧Python
checkpoint/transcript的`ensure_ascii=False`不一致；中文service wire先被canonical
检查拒绝，receipt digest随后连带失败。该次不是Provider网络或协议资格结果。
原始选择器输出见本次会话命令结果；修复已改为保留UTF-8，需按主机默认`-j4`重建并复验。
详细进度见[R4-B4](../specs/182-native-di-python-bindings/evidence/r4-b4-conversation-chain-20260908.md)。
修复后`Spec182Conversation*`按`-j4`增量检查，9 cases/exit0；该结果只覆盖会话组件focused边界。
随后共享回归`Spec182CanonicalJson*`、`Spec182Conversation*`、`Spec182EpochText*`、
`Spec182StreamAcceptance*`、`Spec182Sampling*`共25 cases按`-j4`运行exit0。
移除未使用的会话 ready 常量后按同一 system toolchain/`-j4` 重建 DI shared library exit0，
再运行共享25 cases与`Spec182ProviderHost*` 6 cases均exit0；这些仍是本地 focused/regression
结果，不代表真实两轮跨进程或 T016 qualification。

## 2026-09-08 — Spec182 progress audit checkpoint hook

审计文档checkpoint首次提交exit1：pre-commit扫描全索引中的既有助手引用。
已检查钩子提供的`NDNSF_LOCAL_CHECKPOINT=1`本地模式，仍保留禁止路径检查；
这是提交边界，不是产品测试失败。首边界节录保存在
[commit-failure.txt](../.codex-tmp/spec182-progress-audit-20260908/commit-failure.txt)，
审计结论见[R4-B4](../specs/182-native-di-python-bindings/evidence/r4-b4-conversation-chain-20260908.md#progress-and-feasibility-audit)。

## 2026-09-08 — Spec182 conversation oracle canonical mutation

R4-B4 离线oracle author首次 exit1：负例使用默认 json.dumps，旧参考先以
ConversationCheckpointInvalid("checkpoint is not canonical")拒绝，未到摘要篡改检查。
改用参考 canonical payload 后篡改字段，接收该参考的两类拒绝异常；不是原生运行失败。
详细边界及重试结果见[R4-B4](../specs/182-native-di-python-bindings/evidence/r4-b4-conversation-chain-20260908.md)。
修正后生成/确定性重现及每例错key/篡改检查PASS，author边界RESOLVED。

## 2026-09-08 — Spec182 epoch text test optional payload

R4-B3 增量构建 exit1：新增 C++ 测试直接 parse optional finalPayload，类型不匹配。
生产 coordinator 编译完成，测试尚未执行。增加 has_value 必需断言后解引用；
保留[原始编译日志](../.codex-tmp/spec182-r4-b3/build.log)，同树增量续建到 r2 日志。
进度见[R4-B3](../specs/182-native-di-python-bindings/evidence/r4-b3-epoch-text-20260908.md)。
修复后 unit/DI库增量构建及24 cases/411 assertions PASS；本地测试类型问题 RESOLVED。

## 2026-09-08 — Spec182 CLI probe output-path correction

R4-B2 CLI smoke首次使用build/DI_NativeRequester，实际Waf输出为build/examples/DI_NativeRequester；
探针在进程创建前FileNotFoundError，未执行产品。按build.log Linking行修正探针路径，
不修改或重编代码。原始[路径诊断](../.codex-tmp/spec182-r4-b2-r3/cli-path-error.json)；
续检查结果见R4-B2的cli-record.json。本条不改变已通过的C++测试结果。
修正后CLI help/ldd exit0，实际加载本次DI库，local probe RESOLVED。

## 2026-09-08 — Spec182 stream replacement fixture topology identity

R4-B2新ABI build PASS/879.773s，generation options 2 cases/21 assertions PASS；
stream 5/7 cases、104/106 assertions PASS，两个replacement case在构造Provider B
测试offer时抛topology Provider mismatch。fixture仅改了offer.provider，嵌套topology
仍指向Provider A；此边界发生在新offer签名前，不是Provider重算或恢复协议失败。
保留[stream原始日志](../.codex-tmp/spec182-r4-b2/stream.log)和同目录build/options/stream
record。下一步修fixture绑定后增量编译并重试两个失败case，不放宽生产身份校验。
进度见[R4-B2](../specs/182-native-di-python-bindings/evidence/r4-b2-stream-production-20260908.md)。
r2修复及r3补验均PASS；最终stream 7/7 cases/190 assertions，两个SDK恢复wire PASS，
本地fixture问题RESOLVED，不扩大为真实Provider重算资格。

## 2026-09-08 — Spec182 stale dependent projection oracle

r5 build PASS /30.971s；共享测试130/131通过、2963/2975断言通过，1个投影 oracle
用例12断言失败，无 SIGSEGV。placement-v3 oracle 已改为 intent identity，但依赖它的
projection-oracle.json 未重新生成。核对既有 SDK author 从 placement core_digest
独立计算 plan/dataflow 后重生成；不从 C++ 输出抄期望值，不重编无源码变化的二进制。
[原始失败](../.codex-tmp/spec182-r3-b1-r5/focused.log)，单例重试与后续 oracle 见 R3-B1。
后续 r6 失败单例282/282断言PASS；累计131 DI/13 Core cases及三个独立oracle已通过。
本地批次修复结果见 [Final Local Result](../specs/182-native-di-python-bindings/evidence/r3-b1-request-lifecycle-20260908.md#final-local-result)；不授予T016或真实网络资格。

## 2026-09-08 — Spec182 client fixture task capability

r4 build PASS /46.092s；隔离 client 空 ACK/取消例 exit201，requestWire 报
`NATIVE_REQUEST_CONTRACT_INVALID`。fixture adapter 明确只声明 `task`，新用例却请求
`inference`，被正确拒绝。修用例 task 字段，不放宽生产 capability 检查。原始
[诊断](../.codex-tmp/spec182-r3-b1-r4/client-diagnostic.log)；同见 R3-B1。

## 2026-09-08 — Spec182 request projection identity and lifecycle tests

R3-B1 r3 build PASS /126.721s；首轮 focused process 在5.548s 后 SIGSEGV。
此前已有明确失败：NativeMerge 签名 fixture 的旧 model identity 被拒绝，projection builder
仍比较 source content 与新 intent digest，两个新 client 用例提前终止。不能把末尾崩溃
归为某个已通过协议结果。原始 [focused log](../.codex-tmp/spec182-r3-b1-r3/focused.log)
与 focused-record.json 保留；先修正 identity consumer/fixture，并隔离重跑客户端失败
取得结构化原因。检查测试异步回调捕获所有权，避免 fatal assert 后悬空 fixture。
见 [R3-B1](../specs/182-native-di-python-bindings/evidence/r3-b1-request-lifecycle-20260908.md)。

## 2026-09-08 — Spec182 client fixture crypto declarations

R3-B1 r2 build exit 1 / 232.870s：client 新测试直接调用 EVP，却只间接获得 EVP_PKEY
前置声明，缺 `<openssl/evp.h>`。另在测试前静态发现同例旧 adapter.inspect 返回空模型，
无法抵达 Core；改为真实 NativeCatalogModelAdapter。保留
[r2 log](../.codex-tmp/spec182-r3-b1-r2/build.log)，修复后同树增量 r3。无测试结果。
同见 [R3-B1](../specs/182-native-di-python-bindings/evidence/r3-b1-request-lifecycle-20260908.md)。

## 2026-09-08 — Spec182 requester lifecycle test options

R3-B1 fresh build exit 1 / 506.101s，首个编译错误位于新增 V3 lifecycle test：
`NativeInferenceClient::request` 要求显式 options，第五个参数遗漏；同类 client
空 ACK/取消用例也需修正。原始 [build log](../.codex-tmp/spec182-r3-b1/build.log)
与 [record](../.codex-tmp/spec182-r3-b1/build-record.json) 保留。未链接完成、未运行测试。
修复后在同一构建树增量续建，新日志使用 r2；详见
[R3-B1](../specs/182-native-di-python-bindings/evidence/r3-b1-request-lifecycle-20260908.md)。

## 2026-09-08 — Spec182 published manifest issuer binding

CLI/bootstrap 静态审查发现：canonical publication 产生新 manifest，而固定 issuer
allowlist 仅含原 catalog manifest，会在真实 grant 请求处拒绝。已补显式受信 source
policy，经 publication hash 与 model/source/initializer/profile 绑定后签发新 manifest
grant；不扩充任意 manifest 权限。源码与负例未执行，验收保持 PARTIAL；见
[R3-B1](../specs/182-native-di-python-bindings/evidence/r3-b1-request-lifecycle-20260908.md)。

## 2026-09-08 — Spec182 requester Core buffer conversion

R3-B1 限定 `-fsyntax-only` 首次 exit 1（7.278s）：NativeRequestPlanner 将
std::vector<uint8_t> 直接赋给 ndn::Buffer，两个 Core assignment 字段编译失败。
尚未生成二进制或运行测试。保留
[原始诊断](../.codex-tmp/spec182-r3-b1-syntax/NativeRequestPlanner.log)；
改显式 iterator 拷贝并检查 Begin/Response 同类边界，后续独立 r2 目录重试。
批次状态及命令见 [R3-B1](../specs/182-native-di-python-bindings/evidence/r3-b1-request-lifecycle-20260908.md)。
后续 r2 四源码语法检查全部 exit 0（27.272s）；只关闭此类型错误，不代表链接或请求验收。

## 2026-09-08 — Spec182 requester wire and sealer identity mismatch

R3-B1 静态接线审计发现 SDK V2 model_identity_hash 为 ModelRef intent digest，
现有 V3 sealCore 却比较 descriptor.contentDigest；不能把兼容请求改为 content hash
绕过此边界。需区分 source content 与 request intent，并同步 artifacts/core 绑定。
同时 Core cancelStreamRequest 不移除 pending collaboration，RL-3 必须补清理接线。
这是代码审计发现，未运行请求、构建或实验；证据与未执行项见
[R3-B1](../specs/182-native-di-python-bindings/evidence/r3-b1-request-lifecycle-20260908.md)。
后续源码已区分 V3 intent/source content，重生成当前 SDK V3 对照数据；Core 已补
CancelCollaboration 的 pending 清理接口及用例。尚未构建/测试、尚未完成默认 requester
接线，本项保留未验收。

## 2026-09-08 — Spec182 group projection fixture const proposal

R2-B6 首轮 native build exit 1，新增 Provider 交换分配负/正例修改了 fixture 的
const proposal，编译器拒绝 swap/restore；产品两个 cpp 已编译，测试尚未执行。
改为可修改的测试局部 proposal，保留[原始 build](../.codex-tmp/spec182-r2-b6/build.log)，
新 r2 目录增量重试，见 [R2-B6](../specs/182-native-di-python-bindings/evidence/r2-b6-group-projection-20260908.md)。

## 2026-09-08 — Spec182 grant oracle backend argument

R2-B4 native 增量 build 和 37 C++ cases PASS；离线 Python oracle 在 P-256
fixture 的 `ec.derive_private_key` 缺旧版 cryptography 所需 backend 参数时失败，
尚不能授予完整 oracle PASS。保留[首边界](../.codex-tmp/spec182-r2-b4/oracle-error.txt)，
补显式 default_backend 后仅重跑 oracle，不重复 native build；见
[R2-B4](../specs/182-native-di-python-bindings/evidence/r2-b4-grant-production-audit-20260908.md)。

## 2026-09-08 — Spec182 production grant implementation gap

源码审计确认 NativeArtifactPolicyAuthority 仅有 IssuePort 验证包装，未实现 requester
签名/策略/recipient encryption/authority 签名；client 也缺答复认证和真实 Core 发布
生命周期。现有 wrong-recipient 正例只证明转发，不能证明授权。T005-A/B 保持 PARTIAL，
R2-B4 修复批次已登记，见 [源码边界与接续](../specs/182-native-di-python-bindings/evidence/r2-b4-grant-production-audit-20260908.md)。
本项为静态发现的实施缺口，不是运行失败或协议拒绝结果；未启动重建/实验。

## 2026-09-08 — Spec182 state mapping template call

随后 r2 build PASS、70/71 cases PASS。真实 causal ONNX 的 Concat 重复引用同一输入，
source inspector 将 operand 次数照搬为规划消费者次数，触发图重复消费者拒绝。
仅对规划 edge consumer 去重；原始 node inputs、metadata 与 digest 域保留重复次数。
真实源补 2 个 consumer/7 次引用断言，r2 原日志保留，r3 增量验证；同见 R2-B2。

R2-B2 首轮增量构建 exit 1，首边界是 NativeCanonicalRolePreparer::bindStateContracts
泛型 lambda 内 JSON size.get 缺 dependent-template 关键字，尚未进入测试。
补 template 并静态复核，保留原始日志，后续使用独立 r2 目录；见
[R2-B2](../specs/182-native-di-python-bindings/evidence/r2-b2-state-source-binding-20260908.md)。

## 2026-09-08 — Design diagram width

后续 R3 版面已通过，API 门检测两份并发修改的 Tiger supporting docs 与快照
不一致；按现有规则重采样并重建，未放宽源码漂移检查。原始 run 同见下列图解证据。

新增 DI 类图两侧弯曲返回箭头扩大 TikZ 边界，双 PDF 构建成功但版面门发现
overfull 70.85669pt 后退出 1。改为页宽内折线路径；原始构建和后续检查见
[diagram evidence](../specs/182-native-di-python-bindings/evidence/design-diagrams-20260908.md)。

## 2026-09-08 — Spec182 native Merge sealing fixture

R1-B3 incremental build 29.838s PASS，67/68 cases PASS。新 mixed-role fixture 已完成
publication，但调用 V3 sealer 时漏传 inputs.assemblyByRole，准备角色为空导致
DI_NATIVE_ROLE_BINDING_MISMATCH；不是 publication/签名故障。补原始 prepared roles，
不以发布后角色替代验收输入，原始日志见 [R1-B3](../specs/182-native-di-python-bindings/evidence/r1-b3-native-merge-20260908.md)。
r2 build PASS、67/68 cases PASS：已通过重新封存和 grantView，fixture 又将 protected
epoch 配成无 grant 的明文 policy，finalizeSecurity 正确拒绝。补两角色 protected grant
binding fixture，不削弱生产安全检查；下一轮 r3 使用新日志。

## 2026-09-08 — Cleanup Python path compatibility

旧对象清理脚本在删除前检查使用 Path.is_relative_to；系统 Python 3.8 不支持，
因此尚未生成清单或删除对象。改为 resolve().relative_to() 的包含检查后重试；
pip cache purge 已独立成功。清理清单在 .codex-tmp/cleanup-20260908-object-files.json。

## 2026-09-08 — Spec182 role semantics fixture ingress

R1-B2 build PASS，25/26 cases PASS；新 C++ fixture 只填 result egress 未填 input
ingress，candidate.validate 在语义校验之前拒绝。补齐成对入口/出口并在普通候选
场景同时清空；保留原始日志，见 [R1-B2](../specs/182-native-di-python-bindings/evidence/r1-b2-role-semantics-20260908.md)。

## 2026-09-08 — Spec182 shared workflow validation references

工作流集中到共享 skill 后，validate_design 仍在 feature wrapper 查旧标题/诊断
条款，proof-design 也保留旧 anchor，导致结构检查 exit=1。修复规则读取位置和
真实 anchor，保留原门禁并独立重跑 exit=0。该失败与 YOLO 行为验证无关，原始结果
及修复见 [semantic batch evidence](../specs/182-native-di-python-bindings/evidence/t003-yolo-semantic-batch-20260908.md)。

## 2026-09-08 — Skill validator compatibility field mismatch

工作流修订时，skill-creator quick_validate 拒绝已安装 Spec Kit 技能的既有
`compatibility` frontmatter 字段，退出 1；尚未进入工作流语义检查。
保留安装元数据，改用 YAML/必需字段与定向工作流检查；不影响产品资格。
见 [workflow evidence](../specs/182-native-di-python-bindings/evidence/skill-batch-workflow-20260908.md)。

## 2026-09-08 — Spec182 implicit rank publication mismatch

源码审查发现普通候选省略 rank maps 虽可通过 candidate/role validation，
preparation 和实际 canonical publisher 仍用 map.at 读取 degree，会在发布前失败。
统一 implicit rank-one 语义，覆盖 inline/external 与两种 rank 表达；114 cases、
2248 assertions PASS。此为源码契约缺陷修复，不是完整请求或网络资格结果。
见 [publication evidence](../specs/182-native-di-python-bindings/evidence/t003-preparation-rank-20260908.md)。

## 2026-09-08 — Spec182 ONNX graph fixture control

owned graph r1 build PASS，112/114 cases PASS；两个新 case 的 control 缺必需
requireActive，既有 checkActive 在解析前返回 ASSEMBLY_TIMEOUT。此为 fixture 配置失败，
不是实际耗时超限或协议结果。补回调并保留空回调拒绝负例，r1 原样保留，后续见
[ONNX graph inspection](../specs/182-native-di-python-bindings/evidence/t003-onnx-graph-inspection-20260908.md)。
r2 build PASS，113/114 cases PASS；剩余失败为 test reader 读取不存在的 op_type，
维护 GraphNodeView 字段实际为 operation。修正 reader，保留 r2；独立 r3 build 及
114 cases/2212 assertions PASS。

## 2026-09-08 — Design R3 table layout

后续第三轮版面门通过；补可读目标参考后的 pre-build 门又检测到并行 ONNX 源码漂移，
保持拒绝旧基线，核对差异后重采样。该拒绝证明漂移门生效，不是新源代码失败。
第五轮全量扫描期间 ONNX 头文件再变化；新增显式增量扫描与发布前全集 SHA 检查，
仍保留原始解析和完整校验，不以忽略漂移完成文档。

R3 双 PDF 编译成功，版面门拒绝 AC-13 两个带参数方法名在表格列溢出。
修正标识符断行识别并补回归；首轮原始目录保留，不作为产品或最终 PDF PASS。
见 [R3 evidence](../specs/182-native-di-python-bindings/evidence/design-r3-20260908.md)。

最终独立构建 82/87 页、5 项文档工具回归、源码/API/版面 PASS；原失败轮次保留。
内容修订与全量函数语义审计区分，详见证据中的逐章追踪。

## 2026-09-08 — Spec182 Qwen candidate ingress identity

candidate identity r1 build PASS，86/87 cases PASS；完整 Qwen oracle 发现 native
inputIngressRole/resultEgressRole 额外填首末角色，维护 splitter 两字段为空，导致两条
规范字节/摘要断言失败。按实际候选契约保留空值，真实请求的 ingress/终端检查仍归
后续 sealed plan；不能为通过而改 oracle。r1 保留；r2 build 及 87 cases/1246 assertions
PASS，修复与证据见
[candidate identity](../specs/182-native-di-python-bindings/evidence/t003-candidate-identity-20260908.md)。

## 2026-09-08 — Design chapter semantic review

R2 PDF/源码摘要检查曾通过，但逐章可理解性审阅结果 NEEDS_REVISION。
第 53 章生成/KV/会话仅列 cancel；第 59 章混淆 grant 外部单项输入与内部完整策略物化；
目标历史叙述与 TG 冲突。审阅已完成，修订待办，不把文档问题判为产品运行失败。
见 [chapter audit](../specs/182-native-di-python-bindings/evidence/design-chapter-audit-20260908.md)。

## 2026-09-08 — Spec182 resource repair progress owner

文档检查拒绝 T003-C IN_PROGRESS（T003-A/B 依赖未完成）。本轮实际修复 T003-A
共享资源声明，已将执行状态归回该 owner，下游保持 PARTIAL，不越过依赖门。
原始结果及后续验证见 [resource contract](../specs/182-native-di-python-bindings/evidence/t003-resource-contract-20260908.md)。

## 2026-09-07 — Spec182 YOLO fragment source audit

原生 fragment 哈希缺注册摘要与有序节点，且 backend/安全余量/原子 Merge 与维护
splitter 不同。现已修复，实际 Python 对照及 82 cases/1084 assertions PASS；
完整候选身份仍未闭合。此为源码发现，见 [fragment evidence](../specs/182-native-di-python-bindings/evidence/t003-yolo-fragment-20260907.md)。

## 2026-09-07 — Spec182 descriptor build termination

model-descriptor r1 configure 通过；build 进程终态 rc=143，日志停在 74/173，
没有 compiler error 或 Waf 成功记录，现场无残留 waf/cc1plus。终止来源未确认，
不计代码失败或 OOM。保留 r1 后同一新 ABI tree 的 r2 build PASS（403.580s），
r3 最终增量检查及 81 cases/1067 assertions PASS；见
[descriptor evidence](../specs/182-native-di-python-bindings/evidence/t003-model-descriptor-20260907.md)。

## 2026-09-07 — Design R2 duplicate declaration identity

覆盖检查 r1 拒绝同一函数两处前置声明共享 API ID；修复为每个 ID 一条状态并保留声明位置。
API/还原 r2 PASS；首轮 PDF 构建后的严格检查发现三个长标识符溢出，修正断行后独立重建。
最终 r3 双 PDF（65/69 页）、全部目录/字体/输入身份与源码检查 PASS；失败保留，不计产品资格。
之后并行模型描述符修改再次触发源码漂移；r4 核对新字段/规范身份后仅刷新当前基线，保留前后结果。
最终 r4 当前/目标 66/69 页，工具/API/还原/版面 PASS，检查时无源码漂移。
提交前新增 Python 绑定再次触发漂移；r5 核对绑定并用快照补丁验证未提交绑定身份。
最终 r5 全部文档检查 PASS，852 绑定已覆盖，源码漂移为空；机器记录保存精确采样身份。
普通提交被全索引助手引用钩子拒绝；改用钩子显式提供的 NDNSF_LOCAL_CHECKPOINT=1 本地入口，路径检查保留。
这是文档模型边界，不是产品失败。见 [Design R2](../specs/182-native-di-python-bindings/evidence/design-r2-20260907.md)。

## 2026-09-07 — Spec182 node owner fixture

node/state r1 新 ABI build PASS（410.204s），68/69 cases PASS；空角色负例的
测试 helper 在分配节点时对 roles.size()==0 取模，未进入产品拒绝路径。
保留 r1 日志，修复 helper 后独立 r2 build（21.493s）及 69 cases/972 assertions
PASS；见 [node/state evidence](../specs/182-native-di-python-bindings/evidence/t003-node-state-contracts-20260907.md)。

## 2026-09-07 — Spec182 candidate rank fixture

候选校验 r1 build PASS（41.485s），66/68 cases PASS；两个 V3 case 在共享候选
入口拒绝重复 rank artifact。旧 oracle 给两个 rank 复用同一 artifact，违背维护
SplitCandidate 唯一性约束。保留 r1，修复 SDK/native fixture 后独立 r2 build 与
68 cases/836 assertions PASS；
见 [candidate validation](../specs/182-native-di-python-bindings/evidence/t003-candidate-validation-20260907.md)。

## 2026-09-07 — Design API selector

API 契约首轮渲染使用不存在的 ExecutionLease::release；源码 owner 为 ProviderExecutionLeaseTable。
保留首边界，修正选择器后独立重验；见 [API guide evidence](../specs/182-native-di-python-bindings/evidence/design-api-guide-20260907.md)。
选择器修复后 23 契约/53 签名渲染通过；首轮双份 79 页 PDF 有一个长标识符溢出，已定位并独立修复重建。
连续排版后 63 页；目录页码需第三遍编译才能稳定，已增加构建遍数并另建验证目录。
最终 r4 双份 63 页、58 个目录页码、API 声明/绑定和源码还原检查 PASS；文档失败已修复，不计产品资格。

## 2026-09-07 — Spec182 YOLO tensor edge source audit

NativeGraphSnapshot 缺 tensor edges，YOLO planner 按相邻节点合成依赖及节点数预算，
与维护 Python splitter 不一致。另发现 Qwen fragment 额外哈希与 backend family 差异。
上述局部问题已修复，67 cases/818 assertions PASS；T003-A/B 过早 DONE 已撤回 PARTIAL，
完整候选契约仍待闭合。此为源码发现，
不是网络运行结果。见 [tensor edge audit](../specs/182-native-di-python-bindings/evidence/t003-yolo-tensor-edges-20260907.md)。

## 2026-09-07 — Spec182 publication fixture name

publication recertification r1 新 ABI -j4 build PASS（302.273s）；定向测试在构造
稳定工件名字的 fixture 失败：角色自带前导 /，拼接后含 //，被既有 NDN name 校验拒绝。
修正 fixture 名字，保留 r1 失败后独立 r2 build 与 58 cases/710 assertions PASS；见
[publication recertification](../specs/182-native-di-python-bindings/evidence/t008-publication-recertification-20260907.md)。

## 2026-09-07 — Spec182 graph identity source audit

r2 首次定向运行在篡改 graph 的负例断言失败：产品正确抛 invalid_argument，fixture
期待 runtime_error；真实不同图摘要的 SDK core 对照已通过。保留 r2 日志后修正期待，
r3 build 与 58 cases/594 assertions PASS；未更改产品拒绝逻辑。

维护中的 YOLO binding 区分 planning graph 与 canonical ONNX graph，但原生准备/封存
强制两者相等。现显式绑定两种身份并增加不同 digest 的 SDK oracle；此为源码发现。
发布前后 manifest/source 身份转换仍未接通，见
[graph identity evidence](../specs/182-native-di-python-bindings/evidence/t008-graph-identity-spaces-20260907.md)。

## 2026-09-07 — Design source patch whitespace

设计入 Git 的首轮 staged diff 检查因补丁空白上下文行返回 rc=2；改用零上下文补丁，94 文件还原与 staged diff 重验 PASS。
见 [design evidence](../specs/182-native-di-python-bindings/evidence/design-pdf-baseline-20260907.md#git-patch-whitespace-check)。

## 2026-09-07 — Full Design PDF TeX syntax

完整中文设计 r3 因 Repo 标识符裸下划线触发 `Missing $ inserted`，编译 rc=1。
修复后 r4 双份 35 页编译成功，但 abort 标识符仍有 24.15834pt 横向溢出；继续修复排版。
r5 修复完成，双份 35 页编译、无警告/溢出/缺字、文本一致与版面检查 PASS。
原始失败目录与修复边界见 [design evidence](../specs/182-native-di-python-bindings/evidence/design-pdf-baseline-20260907.md)。

## 2026-09-07 — Spec182 publication artifact identity audit

首轮定向验证另发现旧 sealer fixture 的 1 MiB 不满足真实 Qwen candidate 的 2253 MiB
最低预算，在 publication 前拒绝；保留 r1 build/focused 日志，修正 fixture 后独立 r2
build 与 58-case 定向检查 PASS；未降低产品校验。

旧 ensureArtifacts 接收简化 proposal，只检查返回摘要格式与 role cover，未匹配选定工件。
现迁移完整 V3 输入并逐项检查 artifact digest；此为源码发现，无失败运行。
真实 catalog/Repo 与 requester 主链仍未接通，见
[publication evidence](../specs/182-native-di-python-bindings/evidence/t008-v3-artifact-publication-20260907.md)。

## 2026-09-07 — Design PDF long-symbol overflow

Design R0 首次双 PDF 编译成功，但长测试符号导致第 7 页两个 Overfull hbox；
属文档排版失败，原始构建保留。改用允许断行的内联符号后 r2 双份 7 页 PDF 检查 PASS，
见 [design evidence](../specs/182-native-di-python-bindings/evidence/design-pdf-baseline-20260907.md)。

## 2026-09-07 — Spec182 inspection source audit

inspectModel 合成 catalog source name 并丢失原请求完整 model descriptor，缺实际来源证据。
已改 inspection owner 返回来源/manifest，补齐模型与角色绑定；真实 catalog/Repo 接线仍待完成。
这是源码审计，无失败运行；见 [inspection evidence](../specs/182-native-di-python-bindings/evidence/t008-inspection-roles-20260907.md)。

## 2026-09-07 — Spec182 V3 placement test macro

r1 build 在新测试 BOOST_CHECK 的 std::map 模板参数逗号处失败，尚未运行策略。
修复后 r2 build PASS，最终 r4 build/34-case 定向检查 PASS；见 [V3 placement evidence](../specs/182-native-di-python-bindings/evidence/t003-v3-placement-20260907.md)。

## 2026-09-07 — Spec182 signed offer fixture import

authoring 导入 app_sdk.provider 时缺 py_repoclient 搜索路径，尚未执行签名验证。
补齐仓库 Python 路径后重试；见 [core offer evidence](../specs/182-native-di-python-bindings/evidence/t008-core-offer-admission-20260907.md)。

## 2026-09-07 — Spec182 offer admission source audit

NativeOfferAdmission 从本地 policy 合成 Provider 能力，未消费 ACK payload 或验证
offer 自身签名。T008-B 保持 PARTIAL；先补数据解码，再接 Core provenance 与 policy-bound
signature。此为源码审计，无失败运行；见 [offer evidence](../specs/182-native-di-python-bindings/evidence/t008-observed-offer-20260907.md)。

## 2026-09-07 — Spec182 typed shape Provider consumers

r2 构建在 NativeProviderHandler.cpp 的 YOLO 文本 metadata 与 decode tensor identity
两个旧字符串消费者失败。已改显式文本转换及带类型的身份编码；原始 r2 build.log 保留。
见 [typed shape evidence](../specs/182-native-di-python-bindings/evidence/t004-typed-shape-20260907.md)。

## 2026-09-07 — Spec182 typed shape consumer compilation

shape variant 的新 ABI 构建 r1 在 NativeYoloMergeRunner.cpp:335 失败：旧 metadata
字符串拼接未处理整数/符号类型。已定位文本 runner 配置边界，显式转换后独立 r2 重试。
见 [typed shape evidence](../specs/182-native-di-python-bindings/evidence/t004-typed-shape-20260907.md)。

## 2026-09-07 — Spec182 canonical float overflow probe

typed canonical JSON r1 的 DBL_MAX 对照失败：stream 解析溢出后饱和值误判为回转成功，
输出 2e+308。已定位为 helper 缺少 failbit 检查；保留 oracle，修复后独立 r2 重试。
见 [canonical JSON evidence](../specs/182-native-di-python-bindings/evidence/t004-canonical-json-20260907.md)。

## 2026-09-07 — Spec182 T003 placement repair fixture failures

r1 定向测试 exit 201：sealer fixture 依赖无关 residency 排序；新双角色 fixture 少传一个
tensor degree，在 splitter 构造阶段拒绝。已定位 fixture 首边界，修复输入后在独立 r2 重试。
原始 `.codex-tmp/spec182-t003-placement-r1/` 保留；见
[placement repair](../specs/182-native-di-python-bindings/evidence/t003-role-placement-20260907.md)。

## 2026-09-07 — Spec182 T003 placement source audit reopened

NativePlanning.cpp::propose 将全部角色分配给一个 Provider，并按 residency 集合大小
代替目标工件命中排序。既有 rank-one 局部 PASS 未覆盖该缺陷；T003-C 及相关依赖 DONE
已回退 PARTIAL。此为源码审计，没有新的失败运行；下一步修复逐角色独立 Provider 分配。
同时 T004 真实工件/grant 输入修复已完成定向 33-case 验证，完整 wire 仍未完成。
见 [binding repair and placement audit](../specs/182-native-di-python-bindings/evidence/t004-artifact-grant-bindings-20260907.md)。

## 2026-09-07 — Spec182 T004 native Selection wire incompatible

最小 native codec 诊断中，NativePlanSealer::encode 接受字段并输出 386 字节，
生产 nativeSelectionProjectionV3FromJson 在第一 schema 门拒绝。首边界是 DI wire
构造，不是 Core 网络或运行环境。T004-A/父 T004 重开，禁止把旧 7-case PASS 当完整
Selection 证明；修复完整 canonical wire 和真实工件/grant 绑定后再继续 T010。
原始 `.codex-tmp/spec182-t004-wire-audit-r1/` 保留；见
[A8-01 evidence](../specs/182-native-di-python-bindings/evidence/t004-wire-reopened-20260907.md)。

## 2026-09-07 — Spec182 T010-A unit selector rejected

两个 Boost.Test suite 误用逗号组合，exit200，`no test cases matching filter or all
test cases were disabled`；未执行用例，不是 requester 行为失败。原始
`.codex-tmp/spec182-t010a-deadline-r1/client-state.log` 保留，改为分别执行单 suite。
见 [deadline evidence](../specs/182-native-di-python-bindings/evidence/t010-a-deadline-20260907.md)。

## 2026-09-07 — T002-A L0 consumer aborts: freeze() rejects an empty native adapter registry
- **Area**: spec182 T002-A Installed Library Boundary
- **Symptom**: installed-library consumer
  `tests/standalone/spec182-installed-consumer.cpp` (T001-C-frozen L0 carrier)
  ran against the staged prefix and aborted before printing its OK marker:
  `terminate called after throwing an instance of 'std::invalid_argument'`,
  `what(): native adapter registry is empty`, RUN_RC=134 (core dumped).
- **Root cause**: intra-spec182 contradiction. Commit `3afa7492`
  (2026-09-07 01:50, “spec182: add native assembly grants preparation and
  bindings”) *introduced* `NativeAdapterRegistry::freeze()` together with an
  empty-registry precondition, eight minutes before commit `aba90194`
  (01:58) froze `spec182-installed-consumer.cpp` as the L0 carrier, whose
  probe semantics require an empty registry to be freezable
  (`frozen()==true`, `find("missing")==nullptr`). No production caller
  depends on the throw today; the only prior freeze caller
  (`tests/unit-tests/di-native-preparation.t.cpp:51-52`) registers an
  adapter first. At the T002-A stage the installed library deliberately has
  no concrete adapter class yet (T003-A/B add them later), so rejecting the
  empty state makes the installable boundary unusable by any consumer.
- **Fix**: `freeze()` now latches unconditionally; the “at least one
  adapter” precondition, where a caller needs it (e.g. a provider), is
  enforced at the use site by a `find()`-null check, not by the registry
  latch. Evidence records the re-run under the frozen L0 command.
- **Ref**: NDNSF commit `3afa7492` / `aba90194`; run dir
  `.codex-tmp/spec182-t002a-l0-r1/` retained.
- **Lesson**: a frozen executable carrier and its own feature's earlier
  implementation commits can disagree; the later process freeze wins, and
  every frozen carrier must actually be executed once before closure.

## 2026-09-07 — Spec182 tokenizer bridge toolchain unavailable

尝试在本机对固定 Rust tokenizer bridge 做 release 构建时，首边界为
`cargo: command not found`（exit127）；未进入 Cargo 解析、编译或链接，不能把
bridge/ABI 记为通过。保留原始目录 `.codex-tmp/spec182-tokenizer-r1/`；现有
C++ tokenizer 包装和静态检查继续作为源码证据，实际 bridge 构建需在安装了
Rust1.90/Cargo 的同源工具链上重跑。

同一诊断目录的首次手工 Boost.Test 链接遗漏 `-pthread`，`libcrypto` 因此出现
`pthread_*` 未解析；补齐线程库后同一 3 个负例用例 **3/3 PASS**。该次失败是
命令边界，不是 tokenizer 实现或测试失败，原始摘要见
`.codex-tmp/spec182-tokenizer-r1/link-r1.json`。

## 2026-09-07 — Spec182 skill validator schema mismatch

Spark执行包检查时，通用skill quick_validate拒绝两个既有Spec Kit入口的顶层`compatibility`字段；首边界为校验器schema，不是任务执行或native产品失败。保留原格式，YAML/必需字段/profile路由检查PASS；仓库code-design通用校验PASS。实际检查和fallback见[执行包记录](../specs/182-native-di-python-bindings/evidence/spark-execution-preparation.md#validation)。未启动产品构建/测试。

## 2026-09-07 — Spec182 installable DI library build blocked at NAC-ABE ABI

构建 `ndnsf-distributed-inference` 在既有 `ndn-service-framework` 编译边界失败，exit1；`ServiceUser.cpp`/`ServiceProvider.cpp` 调用 `getPublicParamsDataName`、`getPublicParamsDigest`、`clearCache`、`refreshPublicParameters`、`refreshDecryptionKey`，当前安装的 NAC-ABE 头文件没有这些成员。该失败发生在 DI 新对象编译前，不能归因于 Spec182 代码，也不能把本次构建当作库或产品 PASS。原始命令/首边界记录在 `.codex-tmp/spec182-native-build-20260907-r1/`；下一步先固定匹配的 NAC-ABE 头/库工具链，再重跑同一目标。

## 2026-09-06 — Spec182 typed-complex reference conversion

扩展initializer参考提取R1在complex64-typed失败：ONNX1.17 `_to_array`先组合complex值，再以float storage dtype调用np.asarray，抛`TypeError: can't convert complex to float`，exit1。此前模型full checker已通过，尚未产生identity；不是C++算法失败。raw `.codex-tmp/spec182-t001-identity-extended-r1/boundary.json`。下一R2分别提取其他表示并将typed-complex单独记为旧转换缺陷，不把它标成已支持稳定oracle或降低普通numeric验收范围。

## 2026-09-06 — Spec182 native reuse review boundaries

审计发现现有NativeEpochCoordinator将完整decode作为稳定stream前缀，且C++采样的Top-P截断归一化、重复token惩罚与Python reference不一致。固定tokenizers0.20.3/现有byte-fallback fixture诊断exit0：`好`的prefix为`�→��→好`；seed8的Top-P例和重复token Greedy例，Python返回0、native源代码推导为1。首边界是文本提交/采样算法，不是网络或授权失败；未执行native产品。raw `.codex-tmp/spec182-native-reuse-review-20260906-r1/`；完整输入/hash/源码/边界见[native reuse review](../specs/182-native-di-python-bindings/evidence/native-reuse-review-20260906.md)。A7-08/A7-09 OPEN，T001/O-004先冻结处置，T007/T011修复后由T016验收，不将reference诊断计为产品PASS。

## 2026-09-06 — Spec182 legacy ONNX initializer identity boundary

T001独立reference探针（ONNX1.17.0/NumPy1.24.4，未修改graph.py）确认：BFLOAT16 raw_data两次计算的content digest均不等于声明权重位模式，而typed表示正确；STRING相同model digest在两个独立进程产生不同initializer content digest。首边界是旧numpy_helper/object-array归一化，不是网络、授权或原生装配结果。普通12种数值类型的24个raw/typed向量通过。raw `.codex-tmp/spec182-t001-identity-r1/diagnostics.json`，脱敏[durable evidence](../specs/182-native-di-python-bindings/evidence/identity-reference-20260906.json)。O-002/O-004继续冻结稳定身份与兼容处置；不把错误摘要写为正确oracle、不以未运行的C++测试关闭此缺陷。

## 2026-09-06 — Spec182 tokenizer toolchain download transport

R2改为rustc/cargo/rust-std最小组件，首个rustc归档仍在Python3.8 urllib TLS读取阶段以同样错误exit1；保留`rust-r2/boundary.json`与部分归档。R3只切换Node22 HTTPS传输，保持官方来源、TLS验证及SHA256检查，最多一次有界重试；依赖设计与其他T001工作继续，不把下载失败升格为产品阻塞。

R3已恢复：Node22 HTTPS成功下载rustc/cargo/rust-std三组件，官方SHA256全部匹配，exit0。失败边界为旧Python传输路径，未观察到tokenizer代码问题；hash与后续探针结果记录在同一dependency design。

T001依赖可行性所需Rust1.90.0归档下载R1 exit1，在TLS body读取阶段报`DECRYPTION_FAILED_OR_BAD_RECORD_MAC`，尚未校验/解压/安装，更未编译或执行tokenizer。保留部分归档与`.codex-tmp/spec182-t001-dependencies/rust-r1/boundary.json`；新R2目录有限重试并验证官方SHA256，不使用部分文件。ONNX依赖构建独立，不因下载失败重跑。设计及进度见[dependency design](../specs/182-native-di-python-bindings/contracts/native-dependency-design.md)。

## 2026-09-06 — Delivery-only scope correction

用户明确指出本轮任务仅为交付，编译与测试由另一台机器负责。此前本机ABI消费者验证属于超出范围的扩展；立即停止R4 owned构建进程组2531869，不再启动NDNSD构建、unit/integration、Python扩展验证或MiniNDN。R1/R2中断及R3普通Provider构建记录保留，不能外推完整验证PASS。后续构建/测试均TRANSFERRED，不作为交付阻塞项；当前源码包/definition/依赖锁/skills与GitHub发布已完成，见source handoff。

## 2026-09-06 — Waf interrupted signature persistence

R2日志证明R1超时后Waf未保存task signatures，实际从1/318重新编译，不能称为仅续编剩余对象。停止重复R2（SIGINT exit68，78.516s）并确认编译子进程退出，保留同一raw root的`build-r2/boundary.json`。R3以原配置、`-j2`、3600秒上限只选择缺失的`di-native-provider`及必要依赖；R1在同一fresh树已经成功链接的Core/unit/integration/应用保留逐目标证据。最终需补齐全部交付目标并运行测试，不将任何中断轮次记PASS。

## 2026-09-06 — Fresh ABI build runner time limit

新SVS/NDNSD闭包消费者fresh build R1在299/318触发执行器1800秒上限：exit124，1800.081s，`TIMEOUT_AT_RUNNER_BOUNDARY`。没有compiler error，Core/unit/integration及部分应用已链接，仍有native-provider对象未完成；不能把未完整构建记PASS或解释为协议失败。独立验证树 `/home/tianxing/NDN/ndnsf-svs-abi-20260906` 下 `.codex-tmp/svs-abi-20260906-r1/build-r1/` 保留receipt、boundary和完整log；确认无遗留编译进程后以 `build-r2`、3600秒有限上限继续同一fresh `build-abi`，配置和`-j2`不变。

## 2026-09-06 — Source handoff tooling resolved

最终交付工具/模板/旧builder fixture统一R5 **28/28 PASS**；五项共享skill与接收说明链接/语法通过。真实四库包生成、搬迁后verify、固定base摘要及definition render PASS。R1/R2的生成物/离线VERSION.info、canonical workload与历史host-gate fixture边界均已定位并修复，原日志保留。详见 [source handoff checkpoint](../Experiments/TigerCluster/docs/source-handoff.md#checkpoint)。NDNSF新SVS/NDNSD ABI闭包的fresh编译与运行验证仍待完成，不宣称SIF或Tiger PASS。

## 2026-09-06 — SVS offline metadata and transitive ABI closure

真实归档R2在 `LOCAL_SIF_DEPENDENCY_SOURCE_MISSING:VERSION.info` 停止（exit1），raw `.codex-tmp/source-handoff-20260906/package-r2.log`。SVS的该文件由Waf生成且被Git忽略，本机残留版本仍指向旧commit。改为从固定源码的VERSION/GIT_TAG_PREFIX和git describe生成归档内元数据，记录派生来源，不修改源checkout。同时静态查到NDNSD自己构造SVSPubSub，旧NDNSD二进制也是ABI消费者；将其干净源码及pkg-config路径修复纳入锁定和fresh重建，不复用base中的旧库。

旧build-record fixture R2进一步在 `SPEC175_WORKLOAD_NOT_SEALED:Experiments/TigerCluster/jobs/spec175/workload.json` 拒绝，raw `.codex-tmp/source-handoff-20260906/source-handoff-build-record-r2.log`：兼容alias与canonical路径不一致。fixture按canonical封装，真实sealer同时保留legacy镜像路径及canonical身份，生产门保持不变。

## 2026-09-06 — Source archive cleanliness boundary

真实交付包R1在 `HANDOFF_SOURCE_UNTRACKED:examples/example-trust-anchor.cert` 拒绝，未创建bundle：开发依赖checkout带有未跟踪生成物，HEAD与tracked clean不足以证明归档内容。保留原目录，改为三库全部使用精确commit的全新detached checkout准备R2，不放宽sealer的未跟踪源码检查。这是输入来源失败，尚未运行容器编译。

## 2026-09-06 — Source handoff tool fixtures

交付工具统一检查R1为19 PASS / 5 FAIL，原始 `.codex-tmp/source-handoff-20260906/tool-checks-r1/output.log` 保留。五项旧 `test_build_local_sif_record.py` 都在 `HOST_GATE_WORKLOAD_SEED_MISMATCH` 提前退出：fixture引用历史真实G3清单，但运行时校验当前workload。未到达所测definition/label/source边界，不是新SIF构建失败。修复测试为独立临时fixture，保留生产门与负例；不更新历史资格清单冒充当前结果。

模板首轮静态检查曾因开头注释被既有boundary parser识别成第三stage而失败；说明移入builder头之后。随后静态复审发现wheel锁只检查非空会漏掉离线python-ndn依赖，现要求五个固定wheel输入并补缺项拒绝。模板原始R1/R2日志由本轮 [source handoff](../Experiments/TigerCluster/docs/source-handoff.md) 记录；这些静态/fixture失败均未执行Apptainer。

## 2026-09-06 — Merge validation resolved

静态修复后 full unit 759/759、GDB full integration 154/154、current Python 2171 passed /22 skipped；MiniNDN用户撤销、仅新增授权、Provider撤销全部 PASS。PATH启动失败在独立 R2 修复，最初日志不覆盖。Provider场景主动 SIGINT 后重启的旧进程 exit -2，其余应用 exit0。完整身份和范围见 `specs/182-native-di-python-bindings/evidence/merge-validation-20260906.json`；不能把 current Python 范围或三个网络场景外推为历史全套/181最终qualification。

## 2026-09-06 — MiniNDN launcher PATH boundary

合并验证 `minindn-user-revocation-r1` 在 0.644 s 退出1，首边界为 Mininet 启动器找不到 `ifconfig`；编译 PATH `/usr/bin:/bin:/usr/local/bin` 遗漏系统网络工具目录。拓扑/协议尚未运行，不能解释为撤销失败。系统 `/usr/sbin/ifconfig` 已确认存在；MiniNDN root PATH 增加 `/usr/sbin:/sbin`，保留编译器绝对路径约束。原始 `.codex-tmp/merge-20260906/minindn-user-revocation-r1/output.log` 保留；下一轮使用新目录。

## 2026-09-06 — Full integration after static fences

`integration-static-r1` 仍运行时已发现两个首边界：`Spec175NativeTinyOnnxExpiredDeadlineCleansProviderState` 的 provider failure count 不符，以及 `ProductionNativeHandlersRunD2h212ToCompleteOracleResponse` 缺一个角色/最终 oracle。保留本次完整 GDB 日志，先对照新增排队 fence 与原始 deadline/cleanup 状态更新，不能直接放宽 timeout 或删除拒绝断言。定向生命周期、证书撤销及 Controller45项通过不能替代该全模块结果。

## 2026-09-06 — Static repair cross-review lifetime finding

Build static-review R1 主动 SIGINT（exit68，154.920 s）。交叉复审发现新增 handler `!current()` 分支在 Provider 析构排空队列时仍向 Face 投递裸 `this` 的失败回调；析构后 dispatch 存在 UAF。停止当前构建，先在投递前和回调内检查共享 stopping token，并补 queued-handler 析构回归；不得把中断视为构建通过。原始 `.codex-tmp/merge-20260906/build-static-review-r1/` 保留。

## 2026-09-06 — Static review and full-module recount

NFD 定向 R1 进一步定位：提供私有 NFD 后，首边界变为 PUBPARAMS readiness timeout（10.418 s），因为 Controller 主 Face 是 DummyClientFace，不能与独立真实 probe Face 经 NFD 往返。不是仅缺守护进程。12处 policy fixture 启动改为已有 test-access 调用真实 `registerInterestHandlers()`，不设置 ready、不替换策略/签名处理；完整 `start()` 仍由13项 standalone readiness 和真实 NFD 测试覆盖。原始 `.codex-tmp/merge-20260906/integration-nfd-preflight-r1/`，NFD `/tmp/ndnsf-int-ntroyrk7/`，owned child cleanup=0。

Python import isolation R1 **43 passed / 3 skipped**（12.630 s），CLI loader 恢复 `sys.path` 后与 YOLO 联合执行通过；包含路径保持的新回归。

完整 module 日志纠正先前只统计部分 suite 的摘要：integration R3 为 **140/154 PASS、14 failed**，R4 为 **142/154 PASS、12 aborted**。其中 11 项是 Controller fixture 的 Face 尝试连接不存在的私有 NFD socket；另 1 项 certificate revocation 的 Controller 大写 digest 与 User/Provider 小写 digest 不匹配，不能归类为时间波动。原始目录 `integration-r3`、`integration-r4` 保留。MiniNDN 前先修复并重新验证。

当前 Python R1：2158 passed / 8 failed / 22 skipped；八项 YOLO 导入失败，独立 YOLO 19 项通过，正在检查联合执行时的 lazy import 首异常。原始 `.codex-tmp/merge-20260906/python-current-r1/output.log` 保留。Context Mode 的 `session-events` timeline 恢复查询被 guard 拒绝，继续以仓库、原始日志为准。

静态审查发现 User identity prefix retry 捕获局部 `onFail` 引用，构造结束后的失败回调存在 use-after-free；修复后须覆盖延迟重试。

## 2026-09-06 — D2h predecessor boundary / integration R3

定向 R2 使用了 Boost.Test 不接受的逗号连接完整路径，exit 200，未执行协议用例；原始 `d2h-regression-r2` 保留，改用同一 suite 下的 `ProductionNativeHandlersRunD2h*` selector 重试。

完整 R3：90/92 PASS，exit 201，183.621 s，无崩溃。Trace R1 首次失败为 `NDNSF_DATA_V1 HMAC verification failed`：compact segment 的认证预算原由 capability 生成，接收方错误地用可更严格的 Selection edge deadline 恢复 AAD。恢复 capability 的原始认证字段，同时保留 edge 对实际取数的 deadline 限制；D2h 121/212 既有测试正好覆盖两者不同的情况。

R3 已通过原崩溃的 targeted-stream 阶段；D2h 121/212 仍分别只观察到第一阶段 1/2 个角色，未得到完整 oracle response。按后继准入、依赖取数、scope-key 解密顺序使用独立 trace 定位；不扩大超时或删除 oracle 断言。原始 `.codex-tmp/merge-20260906/integration-r3/output.log`。

## 2026-09-06 — Owner wheel closure

R2 的临时环境来源断言仍失败，说明共享第三方 site 的环境不能依赖 pip 默认同版本判定。安装使用 `--ignore-installed` 强制这组本地产物进入临时 venv，并保留具体越界模块路径诊断；不卸载或替换主机包。

全量 Python 首次缺 `conversation.py`；补 SDK wheel 所有权后 wheel-closure R1 进入已声明 cryptography 依赖缺失边界。安装测试原来 `--no-deps` 且空 venv；改为离线复用测试主机第三方依赖，同时强制每个已导入 DI 模块来自临时 venv，保留 wheel 文件不碰撞与卸载后不可导入的断言。原始 `.codex-tmp/merge-20260906/wheel-closure-r1/output.log`。

## 2026-09-06 — Full Python diagnostic R2

后续 current-fixes R1（7 failed / 123 passed）与 R2（5 failed / 53 passed）首边界已定位为旧 fixture 和本机新导出模型与远端固定 registry 的身份差异；显式本地临时 registry 保留严格 hash 校验，修复依据见 [resolution design](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

3079 passed / 128 failed / 39 skipped，exit 1；旧实验目录/冻结基线缺失、显式 native binary/model 输入未设置，以及当前 facade、clock、build-prefix 测试夹具漂移。按既有 `run_spec175_python_gate.py` 的历史诊断与当前兼容性门分工，保留全量失败，不改旧 hash，不重跑历史 SIF/Tiger；当前合并覆盖的 Core/Repo/UAV/DI 测试另行明确选择并修复。原始 `.codex-tmp/merge-20260906/python-r2/output.log`。

## 2026-09-06 — Provider detached fetch lifetime / integration R2

GDB 捕获旧 Provider assignment worker 在销毁后调用 `Face::getIoContext()`；不是当前 targeted-stream 用例的独立失败。改为 Provider 所有的有界 fetch pool，关闭时取消等待、join，排队回调先检查共享关闭标志。原始 `.codex-tmp/merge-20260906/integration-r2/output.log`，完整记录见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

## 2026-09-07 — Spec182 T006-C: worker digest gate rejects every valid sha256 digest

- **Area**: spec182 T006-C Bounded Native Worker
- **Symptom**: Spec182OnnxWorkerProtocol suite — 16 failures across
  `MetadataAcceptsCanonicalEnvelopeRoundtrip`,
  `MetadataRejectsDigestFormatAndPayloadMismatch`, and
  `SubprocessUnregisteredThenRegisteredMatchesInProcess`. The roundtrip case
  showed `check.ok` false with `failureCode = DI_NATIVE_ONNX_WORKER_METADATA`,
  failureMessage `request metadata recipeDigest is invalid`; the child worker
  rejected the same envelope that the in-process validate call produced.
- **Root cause**: `isSha256Digest()` in
  `NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.cpp`
  required `value.size() == 66` and only checked 7+64 = 71 bytes would be
  valid; every canonical digest (`sha256:` prefix + 64 hex) is 71 bytes, so
  the format gate rejected every digest before the payload-digest compare
  could ever run, and the parsed certified slice was never populated.
  Symptom cluster was one root cause: (a) envelope gate METADATA instead of
  ok; (b) digest-payload mismatch classified METADATA instead of RECIPE;
  (c) the real worker child (stale binary, pre-fix digest gate) rejecting
  the envelope with a METADATA error frame.
- **Fix**: size gate corrected to 71 with a comment
  (`"sha256:" (7) + 64 hex`). Diagnostic staging was added and later removed
  from the failing test; after the fix all 21 Spec182OnnxWorkerProtocol cases
  pass against a rebuilt worker binary.
- **Ref**: re-run command
  `./build-nac182/unit-tests --run_test=Spec182OnnxWorkerProtocol
  --log_level=test_suite`; run dirs retained under the suite output.
- **Lesson**: a format gate with a wrong length constant fails every valid
  input silently as "invalid", so an always-rejecting validator can look
  like a roundtrip/envelope bug; assert length against `prefix + N hex`
  rather than a magic total.

## 2026-09-07 — Spec182 T006-C: spec181-assembly-parity fails full rebuild on missing onnxruntime include

- **Area**: spec182 T006-C Bounded Native Worker (full-build regression path)
- **Symptom**: the first full `./waf build` after editing tests/wscript
  recompiled all 635 tasks; `spec181-assembly-parity` (tests/wscript) failed
  compiling `NativeOnnxRecipeAssembler.cpp` with
  `onnxruntime_cxx_api.h: No such file or directory`, because its `use=`
  closure was `... ONNX ...` without `ONNXRUNTIME` while its compile line
  carried `-DNDNSF_DI_ENABLE_ONNXRUNTIME_CPP` and the ONNX prefix include
  (`repo/.codex-tmp/spec182-t001-dependencies/onnx-install/include`) only
  ships ONNX 1.17, not the runtime headers (`/opt/onnxruntime/include`).
- **Root cause**: the spec181-era target predates the spec182 unified
  runtime closure; every sibling DI target
  (`spec181-protected-runtime-closure`, `spec182-installed-consumer`,
  unit-tests) already lists `ONNXRUNTIME`. The target had not rebuilt since
  the runtime include became mandatory, so the failure surfaced only when a
  wscript change forced a full re-signature.
- **Fix**: add `ONNXRUNTIME` to the `spec181-assembly-parity` `use=` string
  in tests/wscript (closure drift, no behavior change). Full build green in
  11m51s at `-j2`.
- **Ref**: re-run command `./waf -o build-nac182 build -j2`.
- **Lesson**: waf content signatures mean an obsolete target stays green
  until any wscript change forces a full re-signature; after configuring on
  the unified dependency closure, audit remaining targets that compile
  adapter sources without `ONNXRUNTIME`.

## 2026-09-06 — Python collection and generation fixture R2

Python collection 缺 Repo binding 和三个既有辅助源脚本；integration 多 Provider generation fixture 的新 input endpoint digest 与旧常量冲突。分别补构建闭合/输入脚本和独立 endpoint identity，保留首边界证据。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

## 2026-09-06 — Real-NFD readiness collector R1

实际两轮启动/独立 Face round trip 已发生；旧 collector 要求 challenge Data 名后还有 `/`，与新 exact reply 不符，exit 1。修正 exact token 匹配并重跑，不以 collector failure 推断协议结果。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Controller readiness / versioned NAC parameters

Readiness R1 在 PUBPARAMS 首边界超时，exit 124。旧随机后缀探针与新 NAC 固定 generation 名称不兼容；分离 Authority fresh challenge 和当前版本参数验证，不回退 NAC 的版本真实性约束。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

## 2026-09-06 — Integration fixture build R4

D2a fixture 修订时保留了该 case 未定义的 `selectionObserved` 标志，编译拒绝；删除无关赋值后 R5 重建。Unit R2 已 751/751 PASS，不能替代 integration。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Native executable link and full integration R1

Native target 缺 RuntimeStatusStore source；全量 integration R1 60/92 PASS 后以 139 退出，包含 targeted stream memory fault 和旧 wrapper 导入。先补 target source、重建同源 binding 并用 GDB 定位崩溃。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Merged legacy ingress integration R1

完整 suite 中旧 ingress 流程报告空 assignment、错误 role 和 D2b 未完成；保留原始 R1 后定向检查 Selection 首边界。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Merged full unit R1

747/751 PASS，4 failed（2 aborted），exit 201。空 tensor overflow 检查除零、manifest size 和选中 Provider 加密输入流程失败；逐项定位，不把旧失败当作允许项。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Merged SVS catch-up arguments

NDNSF build R1 捕获混合调用：catch-up 方法名已恢复，但自动合并保留旧方法的 bool 参数尾部。恢复调用方已有的数量和毫秒年龄实参；保持现有接收与权限语义。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

## 2026-09-06 — NAC installed pkg-config paths

首次 CMake 配置的 `.pc` 在 GNUInstallDirs 之前生成，include/lib 错指 prefix 根。显式 Waf prefix 不足以保证 Python 绑定使用新依赖；调整 NAC 初始化顺序并验证 fresh-config 导出。运行代码未变，完整 case 结果保留。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md)。

## 2026-09-06 — NAC AttributeAuthority test identity isolation

依赖 full test 41/46 PASS；五项 fixture 默认 Face 读入主机旧 PIB/TPM，尚未测试授权行为即签名失败。显式传入已有内存 KeyChain，保留断言并重跑完整 suite。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Merged Context Mode fixture contract

合并 guard 后 42 PASS / 3 FAIL，首次边界为旧 Claude fixture 的 platform/registry 配置；更新有效 fixture，保留缺 hook/缺 flag 的失败断言。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md)。

## 2026-09-06 — Integration dependency preflight R2

Boost 路径修复后，NAC test link 仍调用 Linuxbrew ld，系统 OpenSSL/dl 符号解析失败。固定系统工具链再构建；原始 R2 日志和首边界见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#build-preflight-r2)。

## 2026-09-06 — Integration dependency preflight R1

NAC-ABE test link 选中缺失的 local Boost 1.82 库；NDNSF configure 拒绝尚未安装的目标 NAC prefix。均为构建前置失败，不是协议结果。先固定系统 Boost 并完成依赖测试安装，再重跑 NDNSF configure。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#build-preflight-r1)。

This is the repository-level index for failed, blocked, and `UNQUALIFIED`
attempts. It is an engineering memory, not a replacement for the active Spec,
the source tree, or a raw run directory.

## Read-first rule

At the start of every substantial task, read the newest entry in this file and
open its durable evidence record. If the entry names a raw log under the
ignored workspace temporary directory, inspect that log with targeted
`rg`/`tail` queries before choosing the next command. Then read the applicable
documents in
[`architecture-reading-guide.md`](architecture-reading-guide.md).

A failed preflight, startup barrier, or evidence collector is not a protocol
result. Do not retry a later gate, reuse a candidate, or claim a PASS until the
failure's controlling boundary and invalidation effect are understood.

## Current failure index

**Experimental consolidation closure (2026-09-06): development checks PASS.**
原生合并 `c770f18b` 的unit759/759、integration154/154、current Python
2171 passed/22 skipped和三个MiniNDN场景均PASS；新目录关联工具162 passed/
3 skipped，新Tiger工具58/58 PASS。下方collector RED由精确证据核对和进程组
清理修复关闭；原始失败保留。Tiger Local R8和B003运行验收仍未关闭，用户已暂停实验。
见 [integration closure](../specs/182-native-di-python-bindings/evidence/integration-20260906.md)
及 [Tiger baseline](../Experiments/TigerCluster/docs/two-node-baseline.md)。

**Tiger baseline collector review R1 (2026-09-06): expected regression RED.**
新增ACK/provider/request/selection与wrong-root边界证据负例在旧collector上
19 failed / 13 passed / 20 deselected，0.23s，证明它会接受不完整或矛盾证据。
这不是网络结果；保留 `.codex-tmp/merge-20260906/tiger-baseline-collector-red-r1/`。
修复后相关unit必须通过，B003实际运行仍未完成；见
[baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md)。

**Tiger two-node baseline Local R8 (2026-09-06): wrong-root boundary mismatch.**
All normal service, permission-rejection and cleanup checks passed. The wrong-root
child rejected PUBPARAMS authentication with abort134 before permission delivery.
Preserve this FAIL; classify only this exact isolated authentication abort in the
next run, rejecting unrelated crashes/timeouts. See the
[baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R7 (2026-09-06): observer/lifecycle FAIL.**
The service returned correct ECHO, but generic V2 binding leaves authentication
metadata unset. Replaced the unavailable-field assertion with observed native
ACK/Selection plus an actual wrong-root rejection obligation. The old Controller
wrapper cannot join its infinite native loop; use the existing C++ executable.
See the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R6 (2026-09-06): permission bootstrap FAIL.**
Raw signed roundtrips, signature negatives and PUBPARAMS succeeded. Controller
lacked public target certificates in its PIB and refused permission encryption;
added public-only imports with ndn-cxx readback and unchanged private-key checks.
Also isolated session state and corrected TERM ordering for FUSE-backed containers.
See the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R5 (2026-09-06): pre-NFD import FAIL.**
The image exposes UnixFace in stream_socket, not stream_face. Runtime preflight
stopped both workers before NFD startup; corrected the actual module path.
See the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R4 (2026-09-06): probe import FAIL.**
Multiline NFD/route configuration succeeded on both local instances. The probe
used KeychainSqlite instead of the image's KeychainSqlite3; corrected the symbol
and moved full application imports into pre-NFD inspection. See the
[baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R3 (2026-09-06): NFD management startup FAIL.**
Both NFDs aborted during internal FIB registration (10021). Review found compact
INFO list entries could not preserve privilege/policy nodes; restored multiline
INFO generation before retry. No protocol result; owned processes reaped. See
the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R2 (2026-09-06): identity setup FAIL.**
ndnsec refused root certificate installation into a nonexistent role-local root
identity. The validator already loads the public root file; removed the redundant
PIB installation. No NFD started; private state was cleaned. See the
[baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R1 (2026-09-06): host preflight FAIL.**
The local Python lacks str.removeprefix; version parsing stopped before identity
or NFD startup. Replaced it with an explicit prefix check and slice. Raw output
and subsequent attempts are indexed in the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger review sync R1 (2026-09-06): 58/58 tool checks PASS.**
Adopted the reviewed supervisor fixture expectation for the existing
collector-before-terminal-validation order, retaining FAILED/cleanup checks,
and restored sys.path after profile-test import. The 51 prior checks plus
7 collector positive/negative checks pass; the migration R1 assertion failure
is closed. No production runtime or cluster was run. See [review sync evidence](../specs/182-native-di-python-bindings/evidence/tiger-directory-migration-20260906.md#review-sync-r1).

**Tiger directory migration unit R1 (2026-09-06): 50 PASS / 1 FAIL.**
The supervisor no-result unit expects TERMINAL_RESULT_MISSING but receives
CollectionError from the collector that now runs first. All 64 moved files
retain their original bytes and modes. The same named test with identical
bytes in a separate physical pre-migration layout reproduces the same failure;
this remains a baseline tool/test issue, not a relocation regression.
No SIF or Tiger execution occurred. See [migration evidence](../specs/182-native-di-python-bindings/evidence/tiger-directory-migration-20260906.md).

**Spec181 delivery-tool counterfactual R1 (2026-09-06): expected semantic RED.**
The 40-check baseline passes. Removing only the exit-code rejection makes the
same named regression fail with DID NOT RAISE, proving it detects false PASS
despite a nonzero child exit. Preserve the mutant and restore the production
check before final validation. Real T008 qualification and sealing remain open.
See [delivery tool evidence](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t009-delivery-tool-20260906.md).

**Spec181 T008 exporter adoption R1 (2026-09-06): focused PASS.**
The isolated exporter/adapter/numerical run passes 41 checks with no skips.
Explicit checkpoint input, actual 32/640 ONNX export, registered signatures,
640 CPU ORT versus PyTorch oracle, and production User negative branches are
covered. Preserve 22 fixed-shape export warnings; candidate local-delivery
tool closure and complete qualification remain open. See
[exporter adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#exporter-adoption-r1).

**Spec181 T008 input closure R3 (2026-09-06): focused PASS.**
The isolated inventory/supervisor run passes 92 checks after the actual RED.
Checkpoint files and registry-referenced public keys are bound; changed inputs
are rejected before children. The shared wrapper output-source dependency is
included with explicit CLI precedence and missing-input rejection. Three
registered Ed25519 public keys pass digest/identity checks. Full qualification
remains open. See [R3 closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#input-identity-closure-r3-and-public-material).

**Spec181 T008 input identity R2 (2026-09-06): missing shared CLI dependency.**
All new input checks pass; the isolated two-file run has 89 PASS and one FAIL.
The existing streamed-generation wrapper regression exposes an unadopted
runner-owned output-directory option. Preserve its CLI failure, adopt only
the output-source/default rejection behavior, and retain the existing test.
See [R2 boundary](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#input-identity-closure-r2-and-shared-wrapper-dependency).

**Spec181 T008 input identity R1 (2026-09-06): semantic RED.**
Fourteen focused checks expose unbound checkpoint and registry public-key
files; five existing drift checks pass. The actual gate attempts its child
entry after either new input changes, caught before a real child launch.
Repair the inventory input owner; no full suite or network case ran. See
[input identity R1](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#input-identity-closure-r1).

**Spec181 T008 assembly closure R4/R1 (2026-09-06): focused PASS.**
The corrected shared-source registration builds both complete C++ test targets.
Only the three named assembly cases ran: 3/3 cases and 138/138 assertions pass
through actual CPU ORT load/warmup with bound Provider/model/plan identities.
The complete suite remains blocked on remaining source/input closure. See
[assembly closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#assembly-test-build-and-focused-closure).

**Spec181 T008 full test build R3 (2026-09-06): source-registration link failure.**
The migrated assembly test compiles, but its shared preparation implementation
was added to grant_sources instead of di_integration_sources. Move that
registration; no suite ran. See [assembly migration](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#assembly-test-api-migration-plan).

**Spec181 T008 full test build R2 (2026-09-06): stale assembly-test API.**
The ProtectedRuntime migration compiles; integration compilation now stops at
ndnsf-di-native-assembly.t.cpp:341, which calls unavailable runtimeMetricsSnapshot().
No complete suite ran. Preserve real ORT load/execution proof while migrating
the check to the current runner contract. See [full build R2](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#full-test-build-r2).

**Spec181 T008 adoption C R1 (2026-09-06): unadopted GPU-source dependency.**
Fifteen checks stop at fixture compilation on the absent CudaDeviceIdentity header; one
source assertion exposes the old GPU metadata path. Inspection confirms this
draft targets uncommitted CUDA/profile changes. Preserve it for T010/T011
preparation, retain CPU-only local claims, and carry the concrete GPU evidence
gap into delivery. No production source was changed. See [Batch C](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-c).

**Spec181 T008 test adoption B R2 (2026-09-06): focused PASS.**
All 19 application/Merge checks pass without skips using explicit signed
canonical inputs. The stale timeout assertion follows the existing request
budget contract; no production deadline changed. Five local test/tool
dependencies remain. See [Batch B](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-b).

**Spec181 T008 test adoption B R1 (2026-09-06): stale source assertion.**
Eighteen checks pass; one expects a historical hard-coded User no-progress
timeout. Inspect its current parameter source before migrating the assertion.
No network attempt occurred. See [Batch B](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-b).

**Spec181 T008 runtime test migration R2 (2026-09-06): focused PASS.**
The final two-file target builds and passes 30 cases / 225 assertions.
Real verified grants now cover publish/fetch rejection and host/device lease
cleanup; credential-free checks remain fail-closed. Full-suite build and
source closure remain open. See [runtime test migration](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#full-test-build-r1-and-runtime-test-migration).

**Spec181 T008 full test build R1 (2026-09-06): stale test API.**
Compilation stops at the old ProtectedRuntime revoke/revoked calls; no full
suite executed. Keep revocation transferred, migrate current fail-closed
tests, and retain dataflow/zeroization assertions using real BoundGrantFixture.
See [runtime test migration](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#full-test-build-r1-and-runtime-test-migration).

**Spec181 T008 test adoption R3 (2026-09-06): focused PASS.**
The final four-file projection passes all 32 checks without skips after
removing the rejected temporary-path fallback. Source/test bytes match the
isolated projection; the complete C++ test-target build continues separately.
See [test adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-a).

**Spec181 T008 adoption checkpoint (2026-09-06): commit hook blocked.**
The staged role-assembly test retained a development-temporary-path fallback.
No commit was created. Remove that fallback, keep explicit input validation,
and rerun the focused checks before committing. See [test adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-a).

**Spec181 T008 test adoption R2 (2026-09-06): focused PASS.**
All 32 checks pass without skips using explicit canonical-package/registry
inputs. The four adopted files cover ACK provenance, truthful negative
verdicts, certified role assembly, and input/terminal ownership. Fifteen
other draft-test dependencies remain to review before complete qualification.
See [test adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-a).

**Spec181 T008 test adoption R1 (2026-09-06): 31 PASS, one input failure.**
The role-assembly regression's hard-coded isolated registry lacks its
catalogue-authority public-key file. Failure precedes assembly at signature
preflight. Bind the test to the explicit package and registry already used by
R19, retaining signature verification. See [test adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-a).

**Spec181 T008 case-configuration R2 (2026-09-06): focused PASS.**
All 76 inventory/supervisor checks pass, including actual per-case child
configuration and pre-launch input-drift rejection. The complete T008 gate
still needs test-source/build closure and final case inputs; R19 remains the
completed T005 subject. See [R2 review](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#focused-configuration-r2-and-review).

**Spec181 T008 case-configuration R1 (2026-09-06): focused RED.**
Eleven assertions expose missing declaration validation, unbound case-policy
bytes, and absent per-case child configuration; four existing input-drift
checks pass. The repair stays in the local inventory/supervisor boundary.
See [configuration preflight](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#focused-configuration-r1).

**Spec181 formal Y-N R19 (2026-09-06): PASS; T008 preflight remains open.**
The maintained CLI completes all seven subcases, including three real grant
variants, on ce6a4ba0. All 63 application child exits are collected; source and
input identities remain unchanged, and no recorded PID or NFD remains.
See [R19 qualification](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-y-n-matrix-current.md#current-r19-qualification).
The next complete local gate needs case-specific configuration binding and
test-source closure; see [T008 preflight](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md).

**Spec181 exact-wire native R1 (2026-09-06): PASS.**
Committed ce6a4ba0 passes maintained native build and independent verify.
Both focused regressions also pass against the refreshed Core (21 and 270
assertions). The affected 12-dimension convergence review restores A05 PASS;
proceed to a fresh R19 matrix, retaining all earlier failures. See
[native review](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#native-identity-r1-and-convergence-review).

**Spec181 exact-tensor R6 (2026-09-06): focused PASS.**
Nine real DI/Provider/IMS cases pass 270 assertions: 1.4 MB compact transfer
fits signed packets (maximum 8,477 bytes), legacy format reconstructs, and
signature/commitment/context/bounds plus authenticated inner HMAC/index/legacy
binding mutations reject at their named boundaries. Commit the shared repair,
refresh full native identity and re-audit before a new formal matrix. See
[wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-repair-r5-and-authenticated-inner-rejections-r6).

**Spec181 exact-tensor R4 (2026-09-06): legacy fixture exceeds packet limit.**
Large compact transfer and four real verifier rejections pass (5/6 cases).
The legacy fixture's 7,000-byte payload segment plus old metadata reaches
10,555 signed bytes; Core correctly rejects before decoding. Use small legacy
segments to test compatibility, retaining the unchanged 1.4 MB compact case.
See [wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-repair-r3-and-negative-probe-r4).

**Spec181 exact-tensor R2 (2026-09-06): test bridge correction required.**
Compact production transfer reconstructs the 1,400,017-byte object and all
signed packets fit; 412/413 assertions pass. The sole failure counts 404
packets versus 202 because both fixture peer bridges and manual bridges run.
Disconnect fixture peer bridges before custom forwarding; keep the same
packet-count, content and size assertions. See
[wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-probe-r2).

**Spec181 exact-tensor R1 (2026-09-06): semantic RED at publication.**
The real DI publishOutput of a 1,400,017-byte tensor creates a 17,546-byte
signed manifest and correctly fails the repaired Core limit. Build passed;
the failure is the remaining codec boundary. Apply compact exact encoding
with authenticated reconstruction and legacy compatibility before retry.
See [wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-regression-r1).

**Spec181 exact-wire Core R2 (2026-09-06): focused PASS.**
Initial inventory rendering separately rejected the new evidence file's missing
layer header; add the explicit scoped header before rerunning that document check.
The production Core rebuild and unchanged Provider/IMS regression pass all
21 assertions: 8,799/8,800-byte signed packets are readable; 8,801-byte packets
reject and a late batch size failure exposes no earlier item. DI compact
representation/consumer repair remains BLOCK before native refresh and matrix.
See [wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#core-repair-r2).

**Spec181 exact-wire Core R1 (2026-09-06): semantic RED reproduced.**
The real Provider/IMS pull regression passes 18/21 assertions but accepts an
8,801-byte signed Data and leaves an earlier batch item readable after a late
oversize item. Adopt full signed-wire prevalidation for the whole batch, then
repeat the unchanged test in a fresh run. See
[wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#core-regression-r1).

**Spec181 R18 diagnosis (2026-09-06): BLOCK at NDN Data wire size.**
The first actual send boundary is now identified: BackboneNeck's exact
MANIFEST Data encodes to 19,658 / 10,883 bytes and a SEG Data to 14,191,
above ndn-cxx's 8,800-byte limit (169 event-loop exceptions). The exact
NDNSF-DI filter exists; downstream deadlines are consequences. Review the
existing compact transport changes and pre-publication size guard as a
bounded shared unit, retaining signature/content commitments. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#exact-dependency-boundary-r18).

**Spec181 T005 formal R18 (2026-09-06): BLOCK at exact dependency transfer.**
BackboneNeck now executes with actual CPU ONNX evidence and completes its
role. DetectShard0/1 cannot fetch its exact tensor manifests; Merge then
times out on their outputs. Inspect publication, cache response and routing
before another run. Source/input identities remain unchanged and NFDs exit.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#exact-dependency-boundary-r18).

**Spec181 backend native refresh R1 (2026-09-06): PASS.**
Committed 214df1d6 completes maintained native build and independent verify;
both report native identity OK. The tested registration change and retained
device/error contracts pass affected convergence review. Resume a new formal
matrix; focused CPU checks do not establish matrix qualification. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-native-identity-r1).

**Spec181 backend repair R4 (2026-09-06): focused PASS.**
The maintained native Provider rebuilds (35.522 s); five real executable
checks pass (1.02 s). Legacy/public CPU names load and warm a real ONNX
model; unknown names and invalid execution-provider metadata still reject.
Commit only registration blocks and tests, then refresh native identity and
re-audit before the next matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-repair-r4).

**Spec181 backend probe R3 (2026-09-06): semantic RED reproduced.**
With corrected evidence assertions, legacy CPU load/warmup and unknown-backend
rejection pass; three public-name checks fail at missing registration. Adopt
only the two registration blocks, rebuild the maintained Provider and repeat
the real executable checks. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-regression-r3).

**Spec181 backend probe R2 (2026-09-06): registration RED and assertion correction.**
Three checks reach missing public backend registration. The legacy backend
actually loads/warms the model, but its test misreads existing string-valued
evidence and nested device fields. Unknown-backend rejection passes. Correct
the schema assertion before the next focused run. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-probe-r2).

**Spec181 backend probe R1 (2026-09-06): BLOCK at test collection.**
An extra parenthesis in the new test prevents collection (0.36 s); no native
process ran. Correct the test syntax and retain R1 before a fresh R2 probe.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-probe-r1).

**Spec181 T005 formal R17 (2026-09-06): BLOCK at backend registration.**
All four Providers pass external assignment validation and enter their handler.
BackboneNeck then fails with no NativeModelRunner backend registered:
onnxruntime-cpu; dependent-role fetch deadlines follow. Preserve that first
boundary, repair actual backend registration and re-audit before retry. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#native-backend-boundary-r17).

**Spec181 digest native rebuild R1 (2026-09-06): PASS.**
Committed e6f44b65 rebuilds the Core library, native Provider and Python
extension; maintained build and independent verify both report native identity
OK. Source/byte checks and exact assignment rejection remain intact. Affected
convergence review PASS; proceed to a fresh formal matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-native-rebuild-r1).

**Spec181 digest repair R3 (2026-09-06): focused PASS; native rebuild pending.**
Four actual C++ helper checks pass (1.73 s) after the isolated Provider emits
canonical lowercase hex. Exact assignment size/digest checks remain intact.
Commit this unit, rebuild the native runtime and re-audit before a new matrix.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-repair-r3).

**Spec181 digest probe R2 (2026-09-06): RED reproduced.**
All four actual-helper comparisons fail only at Provider uppercase hex;
User matches independent SHA-256. Normalize Provider output to the existing
canonical lowercase contract, preserving exact byte/size checks. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-regression-r2).

**Spec181 digest probe R1 (2026-09-06): BLOCK at linker selection.**
The focused helper probe fails at compilation/linking (4 setup errors,
1.60 s), before digest comparison: system g++ selects Homebrew ld from PATH.
Use the system toolchain explicitly and retain the original log. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-probe-r1).

**Spec181 T005 diagnostic R16 (2026-09-06): BLOCK at external assignment validation.**
Core INFO logging shows each Provider receives and queues Selection, then
fails assignment preparation with external collaboration assignment size or
digest mismatch. The duplicate request log is not the controlling boundary.
Inspect assignment publication, fetch and validation on the same source.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#external-assignment-boundary-r16).

**Spec181 T005 formal R15 (2026-09-06): UNQUALIFIED after Selection commit.**
The sealed-plan repair reaches four-role Selection commit, then the User
receives REMOTE_RESPONSE_FAILED. Provider WARN logs contain duplicate request
rejections but no native execution failure reason; this does not yet identify
the controlling cause. Inspect the Core request/Selection boundary and obtain
bounded diagnostic logs before changing behavior. Source/input identities
stay unchanged and all NFDs exit. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-response-boundary-r15).

**Spec181 sealed-plan repair R3 (2026-09-06): focused PASS.**
All 22 sealing/candidate checks (0.97 s) and 36 existing plan integration
checks (0.77 s) pass in the isolated source. Fetch references remain separate
from canonical identity, immutable and digest-bound; malformed values reject
before commit, while legacy/local-preparation defaults remain compatible.
Affected A05 review PASS; resume a new formal matrix after committing. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-repair-r3).

**Spec181 sealed-plan validation R2 (2026-09-06): BLOCK at reference validation.**
The projected field resolves sealing; five normal/legacy/coverage checks pass.
Five malformed reference checks fail because non-string false values and
control characters are accepted. Require strings and reject control chars,
retaining the deliberate empty local-preparation reference. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-validation-r2).

**Spec181 sealed-plan regression R1 (2026-09-06): RED reproduced.**
Ten focused checks reproduce the missing field at the actual V3 sealing
expression (1.72 s). Project the existing shared contract and validate
transport forwarding, digest binding, legacy defaults and invalid references.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-regression-r1).

**Spec181 T005 formal R14 (2026-09-06): BLOCK at sealed-plan reference closure.**
The shared assembly repair permits request planning to reach plan sealing.
The committed SealedCollaborationPlan lacks artifact_fetch_data_names,
already consumed by placement. Review/adopt the existing field and digest
binding, verify plan production/consumption, then re-audit. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-boundary-r14).

**Spec181 canonical binding/assembly repair R6 (2026-09-06): focused PASS.**
The isolated Waf parity target builds successfully; all 40 canonical,
candidate and actual C++/Python assembly checks pass (12.91 s). Actual YOLO
two-candidate binding, recipe and publication-port checks pass as well.
Affected convergence review PASS. Commit only the tested shared CPU unit,
then resume a fresh formal matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-and-assembly-repair-r6).

**Spec181 canonical assembly R5 (2026-09-06): BLOCK at focused harness inputs.**
Thirty-two Python checks pass. Eight native checks lack the required parity
binary; the extended recipe probe incorrectly includes the non-ONNX Merge
role in its graph assertion. Correct those focused harness inputs before
further validation; no network attempt was made. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-assembly-boundary-r5).

**Spec181 canonical recipe R4 (2026-09-06): BLOCK after binding repair.**
R3 actual binding/publication and 24 focused checks pass. Extending the probe
through real role certification reveals that the committed recipe rejects
COMPONENT_SET zero intervals. Add the reviewed component/external-initializer
assembly changes to this source unit; retain unrelated CUDA provider selection
outside the unit. See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-repair-r3-and-recipe-boundary-r4).

**Spec181 canonical-binding regression R2 (2026-09-06): RED reproduced.**
With the complete Python path and private offline NDN environment, actual
YOLO describe reproduces the missing canonical_graph_digest TypeError.
Proceed with the reviewed shared dependency unit and focused checks. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-regression-r2).

**Spec181 canonical-binding regression R1 (2026-09-06): BLOCK at probe import.**
The isolated probe lacks the Repo Python path and stops at SDK import before
the binding constructor. Complete the explicit Python/private NDN environment
for the next focused run. The source/reference contract,
shared deployment consumer and their tests form the reviewed dependency unit.
Validate that unit plus actual two-candidate publication before re-auditing.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-regression-r1).

**Spec181 T005 formal R13 (2026-09-06): BLOCK at canonical artifact binding closure.**
The epoch repair permits the User to request a V3 task. YOLO describe passes
canonical_graph_digest to the committed CanonicalArtifactBinding, whose
dataclass lacks that field. The working tree contains the shared canonical
reference extension, while the isolated commit omits it. Review and validate
the complete binding/publication dependency before another run; all NFDs
exited and source/input identities stayed unchanged. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-boundary-r13).

**Spec181 subcase-epoch repair R2 (2026-09-06): focused PASS.**
All 117 runner/matrix/grant seam checks pass (3.99 s). Child epoch now matches
the publication/Provider runtime inputs; the protected Y-B and all three
Y-N-E mutations retain their grant configuration, and the parent environment
is unchanged. Affected convergence review PASS; T005 needs a new formal run.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#subcase-epoch-repair-r2).

**Spec181 subcase-epoch regression R1 (2026-09-06): RED reproduced.**
Seven plaintext cases inherit the protected matrix epoch; four protected
profiles pass. The focused production runner capture stops before network
startup (7 failed / 4 passed / 88 deselected, 2.06 s). Bind child epoch to the
same runtime inputs used by publication and Provider process specifications.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#subcase-epoch-regression-r1).

**Spec181 T005 formal R12 (2026-09-06): BLOCK at subcase epoch environment.**
Committed fixture loading passes. Y-N-O User inherits the matrix's protected
epoch, although runtime publication and process specifications choose the
plaintext control epoch; its grant seam then raises SPEC181_REQUESTER_PRIVATE_KEY
KeyError. Scope the child epoch to the selected subcase and regression-test
both plaintext controls and protected Y-N-E before rerunning. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#subcase-epoch-boundary-r12).

**Spec181 fixed-input closure repair (2026-09-06): focused PASS.**
The existing PPM and provenance README are adopted without byte changes.
All 22 numerical regression checks pass (7.97 s); the actual canonical
package reference loader verifies the fixture digest and shapes. A05 is
re-audited PASS; verify the committed fixture in the isolated checkout
before resuming the formal matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#fixture-closure-repair).

**Spec181 T005 formal R11 (2026-09-06): BLOCK at fixed fixture source closure.**
The repaired lock permits Controller publication, Repo, and four native
Providers to become ready. User startup fails because the isolated commit
lacks tests/fixtures/spec180/yolo26n/fixed-fixture.ppm. The existing untracked
162-byte fixture matches the manifest digest. Reopen A05, adopt the fixture
and its provenance, validate the real reference loader, then re-audit before
the next matrix. Source/input identities stayed unchanged; NFDs exited. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#fixed-input-source-closure-r11).

**Spec181 T005 formal R10 (2026-09-06): BLOCK at Controller publication initialization.**
The explicit shell environment repaired process spawning. NFD readiness and
Controller startup pass, but the co-located publication ServiceUser constructor
throws `Failed to acquire file lock`. The maintained matrix exits 2 at Y-N-O;
The focused syscall probe finds EACCES before flock: the UID-0 lock path is
owned by UID 1000, with no kernel lock or fuser occupant. Preserve this stale
file in R10 before letting the runtime recreate it; no Core change is needed.
no protocol result is established. Source/input identities remain unchanged,
and all NFD processes exited. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#controller-publication-boundary-r10).

**Spec181 T005 formal R9 (2026-09-06): BLOCK at explicit shell environment.**
The new diagnostic identifies KeyError in Mininet node.py:419: shell=True reads
os.environ['SHELL'], absent from the launch environment. Preserve the exact
frame chain; set SHELL=/bin/bash explicitly for R10. No Controller was launched,
all NFDs exited, and source/input identities stayed unchanged. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-application-boundary-r9).

**Spec181 spawn diagnostic repair R3 (2026-09-06): focused PASS.**
All 93 runner/matrix checks pass. Partial-start cleanup and first-failure stop
remain intact; the new exclusive diagnostic records type and frame locations
without exception text or locals. The actual R8 spawn cause still requires a
new run. See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#spawn-diagnostic-repair-r3).

**Spec181 spawn diagnostic R2 (2026-09-06): BLOCK at test import.**
Two-file checks produce 1 failed / 92 passed (1.55 s). The runtime writes its
new diagnostic; the new assertion lacks the json import. Preserve R2, add the
test import, and rerun. This is a test-fixture failure, not a runtime result.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#spawn-diagnostic-regression-r2).

**Spec181 spawn diagnostic regression R1 (2026-09-06): RED reproduced.**
The existing partial-start cleanup test now verifies a durable error boundary;
it fails because process-start-failure.json is absent (1 failed / 87 deselected,
0.90 s). Keep cleanup and failure verdicts intact; record only exception type
and frame locations, preserving first evidence without messages or locals.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#spawn-diagnostic-regression-r1).

**Spec181 T005 formal R8 (2026-09-06): BLOCK at application spawn diagnostics.**
NFD readiness/routing/keychains pass. Controller log creation is followed by an
immediate spawn failure; the matrix preserves only CONTROL_NOT_PROVEN and drops
the underlying traceback boundary. Preserve R8 and add exception type plus
file/function/line frames, without exception text or locals, before another
diagnostic run. NFD cleanup was verified. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-application-boundary-r8).

**Spec181 T005 formal R7 (2026-09-06): BLOCK at NFD readiness.**
All five NFD sockets exist, but node nfdc checks fail; no application child
started. The launcher inherited offline probe NDN_CLIENT_* overrides pointing
to unused.sock instead of node client.conf. Preserve startup diagnostics and
logs. R8 will isolate the parent via private HOME, remove the global overrides,
and retain explicit Python dependency paths. NFD/native Provider cleanup was
verified. See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-network-boundary-r7).

**Spec181 T005 formal R6 (2026-09-06): BLOCK at Mininet executable readiness.**
The explicit PATH omitted sbin and Mininet could not find ifconfig (exit 1).
Preserve R6. Verify required network tools and append the system sbin paths for
R7 while preserving Python/native resolution order and recording the new launch
environment. This is startup readiness, not a protocol result. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r6).

**Spec181 T005 formal R5 (2026-09-06): BLOCK at launch environment parsing.**
The temporary parser rejected the registered SPEC180_CASE_OUTPUT_DIR in the
Y-N environment. No case/state or runner was created. Preserve R5; accept that
specific field and override it with R6's unique output. The five-role Y-N input
uses the same model; the protected epoch remains explicit for Y-N-E. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r5).

**Spec181 T005 formal R4 (2026-09-06): WAITING_EXTERNAL_INPUT at case roles.**
Envelope ownership now passes. The Y-B baseline configuration lacks Y-N's
required FullModel capability, so maintained validation exits 78 before network.
Preserve R4; inspect and use the existing Y-N-specific inputs for a new R5.
The registered role-set requirement remains unchanged. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r4).

**Spec181 T005 formal R3 (2026-09-06): WAITING_EXTERNAL_INPUT at key ownership.**
The repaired sudo source gate passes on 88e7a458. Maintained input validation
rejects the developer-owned envelope key for root execution (exit 78), before
network. Preserve R3 and provision the same bytes as a 0600 root-owned file in
R4's private state, retaining the original key untouched. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r3).

**Spec181 sudo source repair R2 (2026-09-06): PASS; formal R3 next.**
The gate preserves SUDO_UID only for root plus the actual selected checkout
owner. Real sudo positive/negative tests and existing local gate regressions
pass: 51 checks (8.68 s), with Git overrides still stripped. A05 is re-audited
PASS; T005 remains incomplete and will use a new run directory. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sudo-source-repair-r2).

**Spec181 sudo source regression R1 (2026-09-06): RED reproduced.**
Real sudo Git checks yield 1 failed / 1 passed: the legitimate owner is rejected,
while the wrong UID remains rejected. The test also supplies hostile GIT_DIR
and GIT_INDEX_FILE overrides. Preserve red.log and repair only the matching
sudo-owner identity in the sanitized environment. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sudo-source-regression-r1).

**Spec181 T005 formal R2 (2026-09-06): BLOCK at sanitized source Git.**
Outer Git now accepts the actual sudo user, but production _source_git drops
SUDO_UID again and fails before network. Reopen A05's sudo checkout boundary;
add a real sudo regression and retain the UID only when it matches the selected
checkout owner. All Git override variables remain stripped. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r2).

**Spec181 T005 formal R1 (2026-09-06): BLOCK at launcher Git ownership.**
The explicit environment dropped SUDO_UID; root Git rejects the user's checkout
before invoking the maintained runner. Preserve R1. Retain the actual sudo
caller UID in the explicit launch environment for R2; do not write global Git
exceptions or weaken the source gate. No network started. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r1).

**Spec181 T007 convergence (2026-09-06): PASS; T005 next.**
A05's source/configuration/input/build/application boundaries now map to their
focused regressions and final committed checks. A01–A12 are closed within their
documented scopes. The 12-principle audit permits same-source local validation;
it is not qualification or development-delivery PASS. Historical failures below
remain preserved. See [audit](../specs/181-ndnsf-di-protected-grant-qualification/audit.md#a05-closure-matrix).

**Spec181 committed source R13 (2026-09-06): R12 resolved.**
Repo build intermediates are preserved outside the checkout. The strict source
guard passes for 6b9bb51c, and the real application/native preflight plus four
application imports pass on that commit without network. Overall T007 audit
remains the next gate. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#committed-source-and-runtime-r13).

**Spec181 committed source R12 (2026-09-06): BLOCK at build intermediates.**
The isolated checkout matches 6b9bb51c with a clean tracked/index tree. The
actual source gate rejects untracked Repo setup build/src/ArtifactManifest.o.
Preserve the terminal result and move the complete intermediate build directory
to R12 before retry, retaining runtime extension and strict source checks. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#committed-source-reconciliation-r12).

**Spec181 source closure R11 (2026-09-06): focused failures resolved.**
The same isolated selected source passes 45 existing checks and 12 new candidate
binding checks. Actual application/native preflight and four application imports
pass without network. R1–R10 remain below as historical first-boundary evidence.
Final committed-source reconciliation and T007 audit remain open. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#focused-closure-r11).

**Spec181 focused source closure R10 (2026-09-06): BLOCK at request contract.**
Actual application/native preflight passes without network. Six-file checks
yield 9 failed / 36 passed (1.38 s): missing DIRequestEnvelopeV2 input transport
fields and InferenceApplication task arguments. Preserve R10 and close the two
request endpoints before retry. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-and-focused-r10).

**Spec181 application preflight R9 (2026-09-06): BLOCK at verifier implementation.**
Export alone is insufficient: ProviderOfferTrustVerifier itself is absent from
committed SDK provider.py. Preserve R9 and validate the implementation and its
existing signature/ACK tests as part of source closure. No network ran. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r9).

**Spec181 application preflight R8 (2026-09-06): BLOCK at public verifier export.**
Actual publication/process_specs/native guard pass. The post-guard import probe
then finds user.py requires ProviderOfferTrustVerifier missing from SDK exports.
Preserve R8 and add the existing verifier export; no network ran. Native guard
success alone does not prove application import closure. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r8).

**Spec181 application preflight R7 (2026-09-06): BLOCK at local launch helper.**
Runtime publication passes. Actual process_specs calls legacy python_cmd with
repo/py_dir, which the committed helper lacks. Close the matching local helper
parameterization, retaining default compatibility. Preserve R7; no network ran.
See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r7).

**Spec181 application preflight R6 (2026-09-06): BLOCK at catalogue conversion.**
SDK/Provider imports pass. Runtime publication requires the uncommitted
PreSplitCatalogSnapshot.from_mapping contract. Preserve R6 and close the
matching validation/serialization dependency before retry; no native guard or
network ran. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r6).

**Spec181 application preflight R5 (2026-09-06): BLOCK at Repo reference contract.**
The selected ApplicationInput contract requires LargeDataReference, absent from
committed repo_reference.py. Existing Provider/client/facades already consume
this shared publication/reference owner. Preserve R5 and close that dependency;
no network ran. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r5).

**Spec181 application preflight R4 (2026-09-06): BLOCK at shared input contract.**
Candidate construction now passes. SDK imports then fail because the committed
Provider requires MAX_INLINE_INPUT_BYTES absent from adapters.base; the
coordinator also requires InputTransportMode. Preserve R4 and close the shared
input contract/export dependency before retry. No native guard/network ran.
See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r4).

**Spec181 application preflight R3 (2026-09-06): BLOCK at shared candidate contract.**
Repo's same-source build and import pass. Production YOLO publication then
constructs SplitCandidate with selection_priority, absent from the committed
contract, and fails before native guard/network. Preserve R3; close the exact
shared contract dependency without mixing unrelated worktree changes. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r3).

**Spec181 application preflight R2 (2026-09-06): BLOCK at Repo Python extension.**
Adding four existing YOLO source files to the isolated checkout clears actual
catalogue verification. Policy generation then cannot import
py_repoclient._py_repoclient, which has not been built in that checkout.
No native guard/network started. Preserve R2; use Repo's maintained build
against the same source, not an unknown worktree binary. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r2).

**Spec181 application preflight R1 (2026-09-06): BLOCK at committed Python adapter import.**
The clean a51f87b3 native build passes and emits its new receipt. Actual runner
validate_inputs then fails to import build_yolo26n_adapter from adapters.yolo,
wrapped as CANONICAL_CATALOGUE_VERIFY_FAILED. No network or native guard was
started. Preserve R1 and repair the committed adapter package closure before
retry. See [local runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r1).

**Spec181 local input identity R3 (2026-09-06): BLOCK at new fixture import.**
After R2 66 PASS, new input tests yield 3 failed / 69 passed (7.68 s): three
tests reference json without importing it, before the identity owner runs.
Real-child drift/collection checks pass. Preserve R3, repair the fixture import,
then rerun. See [local input identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-input-identity-20260906.md#focused-r2-and-fixture-failure-r3).
R4 fixes the missing import: 72 PASS (8.40 s). Actual fixture children preserve
completed results and reject input drift before network cases or at final
aggregation. Both CLI discovery paths consume the explicit environment.
Configured input identity unit CLOSED; actual runtime audit remains T007 work.

**Spec181 local input identity R1 (2026-09-06): BLOCK at external input bytes.**
Model/map/referenced-key replacement and package additions reach the forbidden
child boundary after inventory creation (4 failed); launch configuration binds
path strings only. No qualification child ran. Preserve R1 and bind the actual
external inputs in the shared inventory/gate owner. See
[local input identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-input-identity-20260906.md).

**Spec181 explicit config root R1 (2026-09-06): BLOCK at protected launch selection.**
The runner overwrites explicit NDNSF_SPEC180_CONFIG_ROOT with the HOME default.
Absolute/relative overrides and missing-key rejection fail (3 failed / 1 passed,
0.81 s); fixture stops before native preflight/network. Preserve R1 and honor
the explicit root before child HOME changes. See
[explicit configuration root](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-explicit-config-root-20260906.md).
R2 honors the explicit root, resolves it before child HOME/cwd changes, and
rejects a missing explicit key even when the default key exists: 24 focused
checks PASS (0.85 s). Configuration selection unit CLOSED; T007 remains open.

**Spec181 Waf tool identity R3 (2026-09-06): BLOCK at CLI fixture PATH mismatch.**
1 failed / 79 passed (1.81 s): an old CLI linkage test builds with fixture PATH
then verifies with ambient PATH. The new Waf identity check correctly rejects
that mismatch first. Align the CLI fixture environment and retain its original
wrong-Core rejection assertion. R3 log and patch preserved in
[Waf tool identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-waf-tool-identity-20260906.md#cli-fixture-boundary-r3).
R4 repairs the CLI fixture: 80 PASS (1.75 s). Actual Waf directory selection
matches the new owner in both current and isolated checkouts (80 source/resource
files each). Waf source/selection unit CLOSED; runtime/input closure remains
T007 work. Existing native receipts require a maintained rebuild for the new field.

**Spec181 Waf tool identity R2 (2026-09-06): BLOCK at fixture executable identity.**
57 failed / 18 passed (2.77 s): the existing fake interpreter lacks its
executable bit, so real PATH resolution rejects it before mocked build; one
environment assertion also predates child-only WAFDIR. Preserve R2 and repair
fixtures before retry, without relaxing production resolution.
See [Waf tool identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-waf-tool-identity-20260906.md#fixture-boundary-r2).

**Spec181 Waf tool identity R1 (2026-09-06): BLOCK at generated build-tool identity.**
Four waflib content/location mutations escape native verification; interpreter
drift is rejected only after the native probe. Focused fixture checks: 5 failed,
70 deselected (0.36 s), no real build/network/qualification process.
Preserve R1, then bind actual selected Waf implementation in the maintained
native identity owner. See [Waf tool identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-waf-tool-identity-20260906.md).

**Spec181 local configuration R1 (2026-09-06): BLOCK at launch identity.**
Six focused mutations of environment, reserved output variable, declared
digest and interpreter bytes reach the forbidden qualification-child boundary
(6 failed, 0.78 s). The runner already uses explicit environment; the missing
check binds its actual values to the declared digest. No qualification child
ran. Preserve R1 before repairing the shared launch-configuration owner.
See [local configuration identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-config-identity-20260906.md).
R2 binds actual launch inputs through one shared builder/gate owner (52 PASS).
R3 verifies real child environment consumption and post-execution identity
failure (61 PASS). R4 adds builder/gate CLI round-trip and rejects changed
configuration before output/children: 62 PASS (5.80 s). Launch configuration
unit CLOSED; T007 remains BLOCK at runtime/input-byte identity planes.

**Spec181 revision 7 (2026-09-06): BLOCK at local configuration identity.**
Committed-source validation is closed in 2628e3d2 (39 focused checks and
the configured checkout PASS). The remaining controlling work binds actual
local configuration, build/runtime dependencies and development delivery.
By owner decision, SIF/replay/Tiger move to the experiment machine and are
not local closure prerequisites. Git merge is deferred until development ends.
See [scope transfer](../specs/181-ndnsf-di-protected-grant-qualification/evidence/development-scope-transfer-20260906.md)
and [current tasks](../specs/181-ndnsf-di-protected-grant-qualification/tasks.md).

**Spec181 local gate identity R8 (2026-09-06): checkpoint hook rejection.**
The local commit hook rejects assistant-directory references in production
source validation. Remove those non-product exclusions and rerun the focused
checks; do not bypass the hook. No checkpoint was created by the failed commit.
R8 removes the exclusions: 39 focused checks PASS (4.46 s), and the actual
configured checkout passes SOURCE_CHECKOUT_OK. The hook remains enabled.
The retry checkpoint succeeds as 2628e3d2; this hook incident is closed.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md#checkpoint-gate-r8).

**Spec181 local gate identity R7 (2026-09-06): source unit CLOSED; configuration BLOCK.**
All 39 focused checks pass (3.44 s), and the configured 1ba99000 checkout
passes exact source validation. Bad preflight identity has no qualification
child/output side effects; mutation during fixture execution yields
UNQUALIFIED while preserving child/cleanup records. Effective configuration,
generated build-tool/runtime bytes and external import bindings remain open.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md#focused-green-r7).

**Spec181 local gate identity R6 (2026-09-06): BLOCK at Python compatibility.**
New symbolic-link checks use Path.is_relative_to, absent from the maintained
interpreter. Focused checks yield 23 failures / 16 passes; the subsequent
read-only checkout probe hits the same AttributeError before qualification.
Preserve R6; use relative_to with ValueError handling, then require focused
success before the next checkout probe.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md).

**Spec181 local gate identity R4 (2026-09-06): BLOCK at Git LFS representation.**
The configured 1ba99000 checkout is rejected because a committed 134-byte LFS
pointer represents a materialized 240376592-byte release archive. Preserve
R4 before adding exact pointer size/SHA-256 verification; do not classify this
as source tampering or a protocol failure. No qualification child ran.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md).
R5 closes exact LFS-byte verification and passes 33 focused checks (2.51 s).
The next configured-checkout rejection is generated Waf tool code; classify
only its exact generated layout under the separate build-tool identity plane,
whose byte binding remains an A05 obligation. Preserve both R5 logs.

**Spec181 local gate identity R1 (2026-09-06): BLOCK at source authority.**
The real local gate accepts a fixture root without a Git HEAD and an invented
40-character sourceRevision, then reports PASS for six fixture children.
All six exits/cleanup records are collected; no network qualification ran.
Validate actual checkout/source identity before any qualification child or
output directory is created, then retain focused regressions for rejection.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md).
R2 adds real Git fixtures and nine identity mutations; all nine reach the
forbidden child boundary instead of being rejected (9 failed, 0.63 s).
No qualification child runs; preserve the RED log and source patch before
adding checkout, index, tracked-byte and untracked-code checks.
R3 passes all nine rejections and existing gate/inventory checks (23 PASS,
2.39 s). Actual configured-checkout and submodule boundaries remain under
focused review; configuration/candidate identity is not yet closed.

**Spec181 native plan closure R1 (2026-09-06): BLOCK at projection behavior.**
Twelve missing header lines close the maintained local native build, including
the extension import/identity check. Focused plan/merge tests then yield
27 PASS / 2 FAIL: COMPONENT_SET postprocessing is rejected, and two PIPELINE
tensors collide in runtime scope with mismatched producer/consumer names.
Preserve R1 before applying the exact parser/scope repair; no qualification
matrix or model run was started.
R2 closes the parser/scope unit: 29 cases / 133 assertions PASS, including
unchanged transport authorization groups. Refresh native identity from its
source checkpoint before advancing the remaining A05 candidate/config audit.
The 1ba99000 checkpoint subsequently passes the maintained native build,
including a fresh extension import and runtime identity receipt (R3).
See [native plan closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-native-plan-closure-20260906.md).

**Spec181 native source diagnostic R3 (2026-09-06): BLOCK at DI projection declaration.**
A checkout refusal left the first native retry on the prior HEAD plus an
explicit source patch; that run is invalid as clean-commit evidence. It
terminated with two compiler errors: NativeCanonicalOnnxAssembler reads
canonicalArtifactName absent from the committed NativeSelectionProjectionV3.
The nine tested files were then matched to 1df718c8 and checkout completed.
Inspect the declaration and assignment path before a fresh recorded retry.
See [framework source closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-framework-source-closure-20260906.md).

**Spec181 framework source closure R1 (2026-09-06): BLOCK at assignment metadata.**
The isolated six-file dependency closure builds the production framework
library. Its reused lifecycle/assignment checks yield 16 PASS / 1 FAIL:
ServiceProvider loses artifactDataName while projecting a structured
assignment set into CollaborationContext. Preserve the R1 source patch and
result, then close the exact Provider transfer before retrying.
R2 carries the root name through single/structured assignments and rejects
conflicting roots. The isolated target links and passes 26 cases / 204
assertions. This framework boundary is closed; full native closure remains.
See [framework source closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-framework-source-closure-20260906.md).

**Spec181 committed native build R4 (2026-09-05): BLOCK at C++ source closure.**
Clean 6c7a0b23 passes configuration and Waf graph creation, then ServiceUser.cpp
fails to compile: AckAuthenticationEvidence is missing, followed by missing
registration and publish-result declarations. The implementation depends on
uncommitted framework declarations/companions. Preserve R4 and close those
exact dependencies before the next clean build; no protocol result exists.
See [committed native closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-committed-native-build-20260905.md).

**Spec181 committed native build (2026-09-05): BLOCK at Waf graph creation.**
Detached d67de87a configures successfully, but tests/wscript references an
untracked native assembly integration source. find_node returns None and Waf
fails before C++ compilation. No protocol result exists; preserve the isolated
checkout and repair the committed source dependency before retrying.
One focused identity regression also fails: changing loaded tests/wscript
bytes with unchanged mtime does not invalidate the native receipt, because
that Waf control file is missing from source fingerprints.
R2 binds the missing control file (70 identity checks PASS) and builds the
integration target, but its seven named assembly cases yield 3 PASS / 4 FAIL.
All four fail at certified recipe_digest validation before ORT loading;
preserve R2 and compare fixture serialization with the production contract.
R3 corrects the old fixture's quoted integer dimensions, leaving production
digest validation intact: 7 cases / 160 assertions PASS. The missing test
source and identity repair are ready for a checkpoint; a fresh committed
checkout must still pass the maintained native build. T007 remains BLOCK.
See [committed native closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-committed-native-build-20260905.md).

**Spec181 A05 qualification inventory (2026-09-05): CLOSED within focused inventory repair.**
The inherited inventory requires Q-C/Q-W but discovers only Spec180 Python
tests, omitting Spec181 protected-grant regressions. Two tests using the real
inventory builder and pytest collection fail; no formal network run started.
R2 repairs those cases (15 PASS); an old count assertion is corrected, and R3
passes 16 checks. R4 then exposes a second boundary: two tests show rehashed
case-source substitution is accepted by the real builder and gate. These use
fixture children, not a network qualification. Preserve R4 before repair.
R5 rejects both substitutions (17 PASS); the old missing-oracle fixture also
changed its source path and is now correctly rejected earlier. R6 keeps the
registered path while withholding its oracle to preserve that check's scope.
R6 passes 18 checks. R7 then finds three unsafe entry IDs accepted by the
validator although the gate joins IDs into output directories; no escaped
write is attempted. Reject these IDs at the inventory boundary before R8.
R8 passes all 21 checks: active scope/collection, registered case source/args,
safe entry IDs, existing evidence/oracle controls and wrapper compatibility.
The remaining A05 native/candidate effective-configuration audit stays BLOCK.
See [qualification scope repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-qualification-scope-20260905.md).

**Spec181 T007 evidence inventory (2026-09-05): CLOSED within document inventory scope.**
The R003 record reports zero Spec181 evidence files, while the current tree
contains 37. Four active evidence records lack an explicit header layer, and
the audit body still describes already-closed T002/shared-runtime gaps. This
invalidates the old completeness claim, not the linked raw test results.
Four layer headers are repaired; the current inventory covers 145 entries,
including both audit roots and one explicit SELF row. Drift/structure checks
PASS, and all 105 Spec180 evidence-file hashes match the before-scan. Raw scan:
ignored workspace temporary directory `spec181-evidence-inventory-20260905-r1/`.
See [complete inventory](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-evidence-inventory-20260905.md).

**Spec181 shared generation worker (2026-09-05): CLOSED by focused repair.**
Two queued coordinator regressions entered the model after cancellation or
deadline. The existing pre-run cancellation check passed (1/3 cases PASS,
14/18 assertions PASS). Propagating the shared guard through the registered
runtime/worker and state staging repairs the boundary: rebuilt 48 cases /
366 assertions PASS. T007 remains BLOCK for its remaining audit obligations.
See [generation worker authority](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-generation-worker-20260905.md).

**Spec181 shared preparation control R1 (2026-09-05): CLOSED as startup error; R2 focused PASS.**
The isolated launcher omitted the empty output directory; `validate_inputs`
raised `OUTPUT_ROOT_MISSING` before MiniNDN startup. No protocol result exists.
The unified native build passed. R1 is preserved; fresh R2 passed the protected
P-256 control with four verified Providers and seven collected child exits.
See [shared preparation closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-shared-preparation-20260905.md).

**Spec181 T002 native handler observation (2026-09-05): CLOSED; early result INVALID.** A focused test
was started before the repair build completed and ran the previous binary.
R2/green.log is preserved. The original build completed (59.612s), then the rebuilt
binary passed 46 cases / 242 assertions in a new log. The early result is a
validation orchestration error, not a protocol result.
See [native handler closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-handler-closure-20260905.md).

**Spec181 T002 native prepared output binding (2026-09-05): CLOSED (focused repair).** The production
prepared-runner validator accepts a Merge output budget changed from the sealed
Selection's K=300 to K=1. R1 fails one of two assertions after a passing positive
control. Exact output shape/type binding now rejects the mutation; rebuilt focused
checks pass 46 cases / 242 assertions. Source closure remains pending; see
[native handler closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-handler-closure-20260905.md).

**Spec181 T002 native Merge contract (2026-09-05): CLOSED (focused repair).** Direct production-runner
checks pass the numerical controls but expose four missed rejections: unknown
postprocess identity, numeric suffix, trailing shape delimiter, and wrong output
dtype. R1 is preserved (3/6 cases, 22/26 assertions passed). Rebuilt repairs pass
6 cases / 26 assertions; related evidence/readiness checks total 10 cases / 69
assertions. Handler source closure remains open; see
[native Merge closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-merge-closure-20260905.md).

**Spec181 T002 native worker authority (2026-09-05): CLOSED (focused repair).** Four real-worker
regressions show that cancellation/expiry after preparation or during compute
still returns results and retains the registered plaintext lease. Grant
verification is valid initially; the missing boundary is worker consumption.
R1 is preserved. Request guards now fence preparation, compute, cached results,
events, publication, and return; rebuilt focused checks pass 51 cases / 280 assertions.
T002 source closure and unified production rebuild remain open; see [worker authority](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-worker-authority-20260905.md).

**Spec181 T001 request lifecycle (2026-09-05): CLOSED.** Twelve registered-handler
regressions show that cancellation or Selection deadline expiry before/during
grant fetch or after preparation still reaches model execution. Grant expiry
alone does not enforce request lifetime. Preserve the first red run before
repair. Four final regressions also expose a missing comparison between the
grant-reference and Selection policy snapshot. Both repairs pass 151 focused
regressions and six real Python process cases (24 exits collected). All red
runs are preserved. T001 acceptance is complete; T002/T007 remain open. See
[request lifecycle](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-request-lifecycle-20260905.md).

**Spec181 T001/T002 maintained process integration (2026-09-05): focused defects CLOSED.**
R1 native fixtures build, but NFD's own management FIB registration fails
because the test configuration omits management authorization. The requester
then receives connection refused; no grant verification occurred. Preserve
the r1 raw run. R2 fixes NFD but requester bootstrap requires a real Controller
for NAC public parameters. Separately, ten successful P-256 runtime calls leak
24560 bytes / 630 allocations under ASAN despite the Zeroized state. Both
boundaries are recorded before repair. R3 fixes the EC ownership leak: ten
P-256 calls pass ASAN. The real Controller becomes ready, but requester
publication readiness times out before any Provider result; preserve r3
before instrumenting that boundary. R4 NDN logs/stack locate the wait in
ServiceUser construction (NAC decryption key): the fixture needs its own
requester policy and certificate bootstrap. R5 passes five cases; Python's
wrong-recipient rejection is correct, but the test incorrectly requires a
Core wire prefix on an internal typed exception. Preserve r5 and correct
the scoped assertion without synthesizing a Core response. R6 passes all 11
checks (10 real-process cases plus ASAN), with all 40 child exits collected.
T001/T002 full acceptance and T007 remain open. See
[process integration](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-t002-process-integration-20260905.md).

**Spec181 T002 P-256 production path (2026-09-05): focused defects CLOSED.**
R1 proves the runner overwrites an explicitly configured recipient-key map
with the Ed25519 offer-key map. Four other checks stop in fixture key
generation because this cryptography installation requires an explicit backend;
they do not prove a production refusal. R2 fixes the fixture: three actual
entry regressions fail (requester loader, Python Provider loader, map override),
and two wrong-curve rejection checks pass. R3 repairs those entries: 130 checks
pass, but the positive P-256 case reaches the production envelope creator and
fails because its EC key generation also omits the required backend argument.
All three raw results are retained. R4 repairs the production key generation;
134 focused checks pass, including bounded private-file rejection. The unified
native rebuild and a fresh four-recipient P-256 Y-B control pass: four native
grant verifications, terminal numerical match, seven collected child exits,
and empty staging. Full T001/T002 acceptance remains open.
See [P-256 production path](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-p256-production-20260905.md).

**Spec181 T002 recipient credentials (2026-09-05): focused defect CLOSED.**
The production factory accepts only Ed25519 private keys although T002 and
the native verifier also support EC P-256 envelopes. The rebuilt credential
regression runs eight cases; only P-256 loading fails before grant acquisition
with `provider recipient private key is not Ed25519`. The r1 build and RED
logs are retained. R2 loads validated P-256 PEM through the production loader;
all eight focused checks pass, including wrong-curve and permission rejection.
P-256 network acceptance and full T002 closure remain open. See
[recipient credentials](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-recipient-credentials-20260905.md).

**Spec181 T002 helper lifecycle (2026-09-05): focused lifecycle defects CLOSED.**
The native assembler waits synchronously for its helper, ignores request and
operation deadlines during assembly, observes grant cancellation/expiry only
after slow work, and can activate helper output beyond the role envelope.
Six real-process regression failures are retained in
`spec181-t002-helper-20260905-r1/red.log` in the ignored workspace temporary
directory. R2 builds but 14 native checks stop at the loader: the old installed
framework lacks `streamCancelled`. R3 binds the test executable and its
environment to the current build library: 26 focused checks pass. A new
source-fetch cancellation regression then proves that the parent recreates
the erased plaintext directory. R4 serializes protected staging writes
with runtime cleanup; 27 focused checks pass, including the new race. The
new RED log is retained in r3. The final unified native rebuild and a fresh
protected Y-B control pass: four actual grant verifications, three ORT CPU
roles plus native Merge, verified terminal output, and empty staging after
all seven child exits are collected. Full T002 acceptance remains in progress. See
[T002 helper lifecycle](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-helper-lifecycle-20260905.md).

**Spec181 T003 assembly parity (2026-09-05): focused build defects CLOSED.**
The fixed-vector Python lane passes 8 cases; the native lane fails 8 checks
because its required current-source test executable is not built yet. This
is a test-input boundary, not an assembly or protocol rejection. R2 compile
also fails before execution because the manual command omitted the installed
NAC-ABE package's `NAC_ABE_CMAKE_BUILD` definition. R3 also lacks the maintained
framework include path; r4 replaces the manual command with a focused Waf
target using the existing dependency configuration. R4 compiles but exposes
the framework's NDNSD link dependency; r5 adds that configured dependency.
R5 build passes; the final 19 parity checks pass, including real ORT CPU
execution and unchanged negative-cache state. Fixed-vector regeneration is
byte-identical. T003 is complete at its focused scope; T007 remains BLOCK. Preserve
`spec181-t003-assembly-20260905-r1` and build the production-entry fixture
before another attempt. See
[T003 assembly parity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t003-assembly-parity-20260905.md).

**Spec181 T005 evidence preservation (2026-09-05): focused defects CLOSED.**
Five focused checks expose the legacy driver's destructive attempt handling,
runtime-dependent entry, and continuation after matrix failures. No network
processes are started. The repair retires this unsafe automatic retry path
and makes the maintained matrix stop at its first failed subcase. Raw RED
output is retained under `spec181-t005-evidence-repair-20260905-r1` in the
ignored workspace temporary directory. R2 passes 118 focused checks after
retiring the legacy entry and stopping on the first matrix failure. This is
not formal matrix qualification; T005 remains NOT PROVEN. See
[T005 evidence repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-evidence-repair-20260905.md).

**Spec181 T006 positive control (2026-09-05): cold dependency timeout CLOSED.**
All four native grants verify, but r8 Merge's first tensor-manifest fetch
expires at the fixed 10 s no-progress bound while its producer finishes cold
model preparation. User then times out. The three actual grant negatives
pass. After binding the data wait to the configured request budget while
retaining the existing hard deadline and cancellation, r10 completes the
protected native control; a measured dependency wait is 13.97 s. The final
r11/r12/r13 negatives also pass with complete process collection. T006 is
complete at its focused scope; T007 remains BLOCK. See
[T006 production repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t006-production-repair-20260905.md).

**Spec181 T006 production rejection (2026-09-05): false-positive oracle CLOSED.**
Eight focused regressions fail: the configured requester seam publishes no
mutation, invalid mutation/epoch settings are admitted, and User-local
exceptions or markers can masquerade as Provider rejection. No selected
Provider network rejection is established by those old probes. See
[T006 production repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t006-production-repair-20260905.md).
After repair, 62 focused checks pass. The separate runner check retains one
obsolete expectation that a User-local Y-N-E probe should report PASS; that
test is corrected; the updated Python group has 150 PASS. A separate C++
harness link omitted the store source; the next command used a nonexistent
shortened filename. The verified source is NativeProtectedArtifactStore.cpp;
use a new directory for the corrected harness command. The unified native
build has independently completed successfully.
r4 C++ harness passes 22 cases. The r5 live entry stops before network
creation because Y-B inputs omit Y-N's FullModel role. Use the verified
five-role Y-N inputs with an explicit protected epoch for subsequent variants.

**Spec181 T004 lifecycle acceptance (2026-09-05): focused defects CLOSED.**
R1 failed before the waiter because the fixture had no running Controller
serving AA public parameters. R2 corrects that startup order and reaches the
real Provider waiter: an 80 ms wait returns false in about 6 us before run,
with SPEC181_PROVIDER_READINESS_PREMATURE_TERMINAL. The initial non-running
state was incorrectly terminal. R3 retains an OUTPUT_ROOT_MISSING preflight
failure. After the native fix/rebuild, r6 waits 83 ms and starts/stops the real
Provider; r4 reaches Controller readiness at 12.39 s; r5 cancels an active
Core probe in 2.3 ms without hot spinning. All three probes exit 0 after
process collection and network cleanup; six Core checks also pass. T004 is
complete at its focused acceptance scope; T007 remains BLOCK. See
[T004 lifecycle acceptance](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t004-lifecycle-acceptance-20260905.md).

**Spec181 T002/T004 native repair (2026-09-05): focused defects CLOSED; tasks OPEN.**
Retained failures identify group digest format, grant forwarding hint, model
basename, premature readiness, and debugger exit-code boundaries. Live-r7
uses ordinary process commands and the same rebuilt source: native protected
Y-B returns PASS/exit 0, with three ciphertext files and no ONNX plaintext or
staging remnants after cleanup. Production negatives, cancellation/resource
acceptance, source checkpoint closure and T007 remain open. See
[native launch repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-live-repair-20260905.md).

**Spec181 T002 production wiring (2026-09-05): build defect CLOSED; T002 OPEN.**
The first compile failed on the installed ndn-cxx forwarding-hint API. A
separate retained r2 build passes native/library/extension identity checks,
and 3 rebuilt-extension tests consume the 9 grant vectors. Real Provider
network and ORT lifecycle acceptance remains open. See
[T002 production repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-production-repair-20260905.md).

**Spec181 T002 native runtime repair (2026-09-05): focused defects CLOSED.**
18 native focused tests pass with real verification, managed content keys,
deadline/cancellation checks and retryable cleanup. Initial compile, linker
and consumption/deadline failures remain preserved. Factory, storage AEAD
and real network acceptance still keep T002 open.
See [T002 runtime repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-runtime-repair-20260905.md).

**Spec181 T001 registry repair (2026-09-05): focused defects CLOSED.**
100 focused tests plus 7 inherited grant tests pass for pinned registry
policy, private-key matching, distinct issuer/publication identities and
the final published-root allowlist. Initial RED and Python 3.8 compatibility
failures are retained. T001 network and lifecycle acceptance remains open.
See [T001 registry repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-registry-repair-20260905.md).

**Spec181 T001 Provider repair (2026-09-05): focused defects CLOSED.**
70 focused tests pass for authorization before preparation, in-memory keys,
on-disk AEAD loading, model/weights cleanup and registered-handler failures.
Registry-policy wiring and real network integration still keep T001 open.
See [T001 Provider repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-provider-repair-20260905.md).

**Spec181 T001 lifecycle repair (2026-09-05): focused defect CLOSED.** Five
RED failures are repaired; 19 focused tests pass for in-memory key leases,
duplicate protection, complete cleanup and private/symlink-safe files.
Provider integration remains open; this does not close T001.
See [T001 lifecycle repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-lifecycle-repair-20260905.md).

**Active Spec181 audit (2026-09-05): BLOCK.** Native protected runtime wiring,
production-path negative validation and assembly parity remain unproven;
the active Context Mode plan link has been repaired. Latest retained Spec181 Y-B log
reports `CASE_RUNTIME_PROCESS_START_FAILED:control`, not a protocol result.
See [Spec181 audit repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/audit-repair-20260905.md).
Focused repair checks are allowed; full qualification requires a fresh audit PASS.

**Current controlling repair (2026-09-05):** Y-N negative verdicts accepted
unrelated exceptions as PASS. The first focused regression reproduced 12
failures; the repaired User/runner/application/build-guard set passes 183
focused tests. The unified build also exposed a stale legacy RUNPATH in
`build-system-j2`, despite its name. See
[negative verdict and build repair](../specs/180-ack-driven-cross-model-qualification/evidence/t011-negative-verdict-repair-20260905.md).
FR-008 still lacks the production authenticated, recipient-encrypted grant
path and operator-authorized issuer configuration; see
[protected-grant gap](../specs/180-ack-driven-cross-model-qualification/evidence/t008-protected-grant-gap-20260905.md).
Older PASS labels below must not be reused as safety evidence. The r42 startup
observation remains useful but does not close this semantic verdict defect or
the missing FR-008 protected execution path.

| ID | Observed | Scope | First failing boundary | Disposition | Durable record | Raw run data |
| --- | --- | --- | --- | --- | --- | --- |
| `SPEC180-Y-N-R42-EXTENSION-CWD` | 2026-09-05 | Spec180 current-source extension rebuild | `pythonWrapper/setup.py` was invoked from the repository root, so its relative C++ source path could not be found | `CLOSED as command-invocation error`; compiler exited 1 before producing an artifact | [`t011-y-n-live-current-20260905-r42-extension-build-cwd.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r42-extension-build-cwd.md) | no raw run; command output is preserved in the task log |
| `SPEC180-Y-N-R41-I-EXTENSION` | 2026-09-05 | Spec180 focused Y-N-I current-source check | Loaded Python extension/framework artifact identity before interpreting the live protocol result | `CLOSED by r42`; rebuilt artifacts contain the current source and r42 logged the marker sequence | [`t011-y-n-live-current-20260905-r41-extension-check.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r41-extension-check.md), closure [`t011-y-n-live-current-20260905-r42-i-only.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r42-i-only.md) | ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r41-i-only/` |
| `SPEC180-Y-N-R40-PATH` | 2026-09-05 | Spec180 diagnostic I-only | Temporary run path before the maintained helper completed | `CLOSED as command-path error`; the command used a mistyped directory and was interrupted with exit 130 | [`t011-y-n-live-current-20260905-r40-path-preflight.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r40-path-preflight.md) | preserved partial data under ignored workspace temporary `spec180-diagnostic-path-error-r40/` |
| `SPEC180-Y-N-R39-I` | 2026-09-05 | Spec180 local MiniNDN Y-N | Controller `PUBPARAMS` readiness before Y-N-I reached provider execution | `CLOSED by r42 for the focused I-only boundary`; r42 reached readiness and provider execution with the rebuilt current artifacts; the full matrix remains open | [`t011-y-n-live-current-20260905-r39.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r39.md), closure [`t011-y-n-live-current-20260905-r42-i-only.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r42-i-only.md) | ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r39/` |
| `SPEC180-Y-N-R37-P-PREFLIGHT` | 2026-09-05 | Spec180 diagnostic P-only | MiniNDN root/user-namespace preflight before NFD creation | `CLOSED by r38`; diagnostic command omitted `unshare -Urnm` and exited 1 | [`t011-y-n-live-current-20260905-r37-p-only-preflight.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r37-p-only-preflight.md), closure [`t011-y-n-live-current-20260905-r38-p-only.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r38-p-only.md) | ignored workspace temporary P-only attempt `spec180-yolo-y-n-current-20260905-r37-p-only/` |
| `SPEC180-Y-N-R36-P` | 2026-09-05 | Spec180 local MiniNDN Y-N | Controller `PUBPARAMS` readiness before Y-N-P reached the ACK-closed boundary | `UNQUALIFIED`; Y-N-O/C/R/I/E/L passed, Y-N-P was not proven | [`t011-y-n-live-current-20260905-r36.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r36.md) | ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r36/` and console log with the same run-id |
| `SPEC180-Y-N-R35-P-E` | 2026-09-05 | Spec180 local MiniNDN Y-N | Controller `PUBPARAMS` readiness before either negative case reached the ACK disposition path | `UNQUALIFIED`; Y-N-O/C/R/I/L passed, Y-N-P/E were not proven | [`t011-y-n-live-current-20260905-r35.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r35.md) | ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r35/` and console log with the same run-id |

Historically, `r42` was recorded as closing its artifact-identity and focused
readiness occurrence. Its rebuilt framework library and Python extension were checked,
and the I log shows the Controller
issuing `registering prefix: /example/controller` and the six NDNSF filters,
followed by successful root and `PUBPARAMS` registration, the readiness probe,
and the expected Y-N-I execution markers. This is focused T011 evidence only;
the full Y-N matrix and T014 remain open. This does not qualify the subsequently
changed readiness implementation or negative oracle, nor prove the complete
dependency closure now checked by the unified build. The earlier r39 I
occurrence was closed only at that historical boundary; its bytes are preserved.

The earlier `r36` result likewise showed the Controller
issuing `registering prefix: /example/controller` and the six NDNSF filters,
but the first Face connection closes before the root registration completes;
the reconnect installs only the six NDNSF routes. There is no successful
Controller-prefix registration and no `PUBPARAMS` filter before the Python
readiness timeout. This is classified as a startup/transport boundary failure,
not as an ACK disposition failure. r35 remains relevant as the prior broader
P/E occurrence.

The ordinary `ConfigManager` message about a missing `/etc/ndn/ndnsf.conf` in
the child logs is ambient diagnostic noise for this run; it is not the
controlling failure because the successful subcases contain it as well.

## Historical pointers

These records remain useful when the current failure is related to their
boundary, but they do not advance the active gate by themselves:

- [`t013-controller-pubparams-readiness-current-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t013-controller-pubparams-readiness-current-20260904.md): why the real AA `PUBPARAMS` readiness barrier exists and why the old exact-SIF result was invalidated.
- [`audit-revision123-design-code-conformance-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/audit-revision123-design-code-conformance-20260904.md): revision-123 design/code findings and their evidence boundary.
- [`t014-tiger-path-audit-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t014-tiger-path-audit-20260904.md): Tiger-path audit block and the reasons implementation checks were not qualification evidence.
- [`t013-supervision-repair-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t013-supervision-repair-20260904.md): supervision and cleanup caveats retained after the repair.

## Failure-record contract

Every failed, blocked, or `UNQUALIFIED` command that can affect task order
must produce or update a durable record before the next retry. The record must
contain:

1. a unique run/failure ID and UTC/local date;
2. the exact source/candidate/config identity and command;
3. the first failing boundary, exact marker or error, and child exit status;
4. the raw-log path plus a compact, secret-free excerpt or structured summary;
5. the affected Spec task/gate and any candidate/evidence invalidation;
6. the next allowed action and the condition that closes the failure.

Keep large logs, private keys, credentials, and transient sockets out of Git.
Use a new ignored workspace temporary directory named with a unique run-id for
each attempt; never overwrite a prior run. The durable evidence record must be
sufficient to understand the failure when the transient directory is later
unavailable.

## Closing an entry

Change the disposition only after a fresh run reaches the same boundary and
produces the required closing marker. A focused unit test can close an
implementation defect, but it cannot close a live, SIF, or Tiger gate unless
the active Spec explicitly defines that test as the gate's evidence.
# NDNSF Failure Log

## 2026-09-06 — UAV API compatibility audit: static authorization expires without renewal

- **Symptom**: an application with unchanged static permissions can compile with the new APIs but lose authorization at the Controller status's 24-hour boundary. Re-fetching the status does not extend its fixed validity window. Source probes also reproduce seven old-call compilation failures: deleted public large-response helper, three positional aggregates, two Hybrid member pointers and one NAC ParamFetcher member pointer.
- **Cause**: Controller initialization/epoch advance sets `m_policyValidUntilMs`; ordinary `getPolicyStatus`/Interest handling does not renew it, while new runtime enforcement rejects expired status. Separately, the helper removal, inserted fields and overload additions break specific source forms. Asynchronous DKEY bootstrap and ACK payload redaction introduce further behavioral migration requirements.
- **Disposition**: audit only, production fixes remain open. Do not present prior short MiniNDN scenarios as static long-running compatibility acceptance or restore insecure fallback behavior. Prioritize safe immutable-status renewal, production readiness semantics, then source compatibility/migration.
- **Evidence**: `specs/179-request-scoped-confidentiality/evidence/uav-api-compatibility-audit-20260906.md`; `results/api-compatibility-audit-20260906/summary.json` has 40 compilations (17 historical and 6 official controls pass; current 10 pass/7 compatibility failures). Six unmodified historical Apps/examples compile. A separately linked production expiry-boundary probe reports lifetime86400000, allowed-before1, allowed-at-expiry0, `controller_status_expired`, expired-refresh-accepted0. Its exit0 means defect reproduced, not fixed; no actual 24-hour network soak was executed.
- **Lesson**: preserve separate source, ABI, protocol, startup and long-running behavior gates. Applications that do not call new authorization APIs still enter the new runtime defaults.

## 2026-09-06 — API audit probe setup produced false compatibility failures

- **Symptom**: initial typed helper probe failed on both revisions (`std::string` lacks `ParseFromArray/SerializeToString`); NAC probes failed with unknown ParamFetcher/PublicParams. Trying a different include order alone did not solve the NAC failure.
- **Cause**: the synthetic payload did not satisfy the pre-existing template contract. Probe compiler paths allowed NAC algo headers' bare `common.hpp` to resolve to NDNSF's same-named header. These were invalid audit controls, not new library defects.
- **Fix**: use a minimal protobuf-shaped compile-only payload; put the NAC source/package directory ahead of NDNSF paths and use NAC's CMake header mode. Preserve initial logs under `results/api-compatibility-audit-20260906/setup-attempts/`, then rerun the entire matrix with at most two compiler processes. Final historical controls all pass, and the seven current failures are attributable to actual API changes.
- **Lesson**: never call a compiler red an API regression until the identical source compiles on the declared baseline with correctly isolated headers.

## 2026-09-05 — T022 official-merge segmentation regression setup

- **Symptom**: first new Producer segmentation test build fails because the default-template factories are protected. Two guessed inspection paths did not exist; the actual campaign is `scripts/spec179_minindn_campaign.sh` and Segmenter belongs to installed ndn-cxx. Initial T022 placement also made the structure scanner warn about task order.
- **Fix**: use explicit Data templates in tests without changing production access; locate exact files and move T022 after T021. Preserve the failed build log under `results/spec179-official-merge-20260905/nac-red-build.log`; retry at unchanged-j2.
- **Lesson**: test public behavior through explicit inputs; do not expose production helpers for convenience. Use file discovery instead of guessing script paths. This compiler failure is a test setup defect, not the expected pre-merge behavioral red.
- **Behavioral red**: fresh85547eb build plus four new cases gives12/15 pass,3 failed cases/5 failed assertions, exit201. CP/KP ignore128-byte limits (1500-byte data, single CK segment); normalizeCkKey collapses distinct segment-named objects and the second warm-cache decrypt returns wrong plaintext. Existing object/exact-segment retrieval and invalidation/reentry tests pass. Official58f3948 merge is conflict-free; green results pending.
- **Merged green**: Experimentalc3aafa6 retains85547eb and official58f3948 ancestry. The same15 cases all pass, including both segment-limit tests and distinct-CK plaintext. Separate prefix installed with f5cb1ec8 library hash; full NAC/native/network qualification continues.
- **Dependency gates**: full46/46,4284 assertions; installed-prefix26/26,1082 assertions. An initial launcher unittest command treated tests/minindn as an importable package and failed2 imports; rerun with PYTHONPATH=tests/minindn and explicit module names. Preserve the import-error log separately from actual launcher results.
- **Launcher runner correction**: that import-path retry executes0 unittest cases, so it is rejected as vacuous. These are pytest function tests; inspect their declared runner and execute python3 -m pytest on both exact files. Neither the import failure nor the zero-test exit0 is a launcher green gate.
- **T022 closure**: merged15/15, full NAC46/46 (4284 assertions), installed26/26 (1082), clean native183 unit/74 integration (11998/1297), pytest28/28; full18 MiniNDN scenarios,188 assertions and both dedicated User grant gates at clean cdd8e55a.33 hashes match disk, driver/all CLIs exit0. Build28m9.720s at-j2; no runtime source changes beyond the two official NAC files. Preserve the compiler/import/zero-case and behavioral reds. T014 publication is deferred, not failed or satisfied by the local merge.

## 2026-09-05 — NAC upstream provenance inferred from a remote alias

- **Symptom**: the first T014 delivery update called suraviregmi/NAC-ABE the original project and inferred12 missing prerequisites from its stale master.
- **Cause**: verified remote SHAs but did not first verify GitHub fork parent/source metadata. The alias `suravi` did not establish official ownership.
- **Correction**: GitHub API confirms both remotes fork UCLA-IRL/NAC-ABE. Fetching official master58f3948 into an isolated clone shows fork master2 ahead/3 behind and Experimental6 ahead/3 behind. The missing changes are maxSegmentSize propagation and removal of segment stripping, plus their merge. Corrected all current delivery/plan/task/audit claims; the preceding entry is historical and its original-project12-commit statement is superseded here.
- **Evidence**: both isolated no-commit merge previews pass without conflicts, changing only cache-producer.cpp and consumer.cpp; previews aborted afterward. Candidate trees and exact source diffs are recorded in `specs/179-request-scoped-confidentiality/evidence/nac-abe-official-comparison-20260905.md`. No candidate runtime qualification, actual NAC branch update, push or PR occurred. User explicitly requests no PR.
- **Lesson**: establish official repository identity from parent/source metadata, then fetch live refs and compare both ancestry directions. A clean merge proves textual compatibility only. A multi-file documentation patch with one mismatched context was rejected atomically; split it into verified exact-context updates without altering runtime files.

## 2026-09-05 — T014 upstream package described an incomplete, ambiguous delivery

- **Symptom**: the earlier package omitted NAC T019/T020, called personal fork master upstream, mixed ParamFetcher API changes into a no-API-change PR and treated the tested OpenABE worker as independently optional.
- **Cause**: delivery prose was not refreshed after dependency compatibility repairs or verified against actual remote/base ancestry.
- **Fix**: retain the old draft as explicitly superseded; pin all four commits through85547eb, map every public surface, document ABI/lifetime limits, provide a concrete fork PR draft, and distinguish the12 prerequisite commits between original master and the tested base. A standalone bundle passes verification, fresh clone, exact head/tree comparison and fsck. T014 stays open for publication authorization, upstream acceptance and rebuilt gates.
- **Tool deviations**: a query guard rejected low-entropy `T014`; corrected to the exact feature basename before authoritative reuse. Broad CodeGraph exploration returned unrelated version symbols; after the required attempt, exact source-string verification established callers. apply_patch rejected delete/add operations on one path atomically; a separate current report and historical pointer avoid destructive replacement. No runtime files changed.
- **Lesson**: an upstream package must identify the destination, prerequisites and complete tested revision; a local bundle or fork push does not prove upstream acceptance. Use a high-entropy feature identifier and update each path once per patch.

## 2026-09-05 — Provider online authorization was absent from MiniNDN grant coverage

- **Area**: Spec179 T021, Controller service-offering permissions.
- **Finding**: existing network grants hardcode User/B and `/PERMISSION`; Provider component policy assignment and network revocation do not establish first-grant service execution. Controller example's grant timer cannot select `/SERVICE`, and Provider example lacks the User example's explicit post-startup permission renewal.
- **Repair**: explicit User/Provider grant-role option (default User), Provider App-owned renewal timer, separate Provider normal/late first-grant scenarios with targeted traffic and unaffected control. Pure evaluators retain target/control failures and reject early service, wrong role/provider, absent renewal and missing late timeout ordering.
- **Evidence**: new evaluator before implementation fails11/11 because no evaluator exists (`provider-grant-evaluator-red.log`); after implementation11/11 pass. Combined launcher gate initially25/26 passes; sole failure is the old exact User-only guard-error string. Parameterize the host-namespace guard test for both roles; final27/27 pass (`provider-grant-launcher-final.log`). Native rebuild and network results pending under `results/spec179-nac-compatibility-20260905/`.
- **Lesson**: Controller policy mutation, runtime permission installation, key readiness and actual service execution require distinct evidence on each role. A User grant cannot qualify Provider service-offering authorization.
- **First network red**: normal and late Provider runs both complete but exit4/gatefalse (`provider-campaign-first/`, driver exit1); User compatibility control passes. Benchmark paths ignore `--known-provider-ids`, so Provider/A serves requests intended for B, including pre-grant successes. User compares provider/service records when deciding DKEY refresh, so an added Provider route incorrectly refreshes an unchanged User service attribute. Normal control31/32 and target31/32 also retain a transition timeout. Provider status-advance permission revalidation legitimately precedes the manual timer in the normal case, contradicting the initial test assumption.
- **Follow-up repair**: benchmark calls use the existing explicit-provider overloads, preserving built-in/custom selection; User DKEY change detection compares service sets, with an added route-only Controller integration regression. Evaluate actual post-grant Provider permission-fetch events and the later idempotent App timer separately. Use the existing grant probe's250ms status cadence; retain all terminal failures and document that version changes can cancel in-flight work, rather than claiming uninterrupted traffic at arbitrary timing. Rebuild/native/network verification pending.
- **Repaired native checkpoint**: build exit0 in4m27.010s at-j2; six dependency closures pass; launcher28/28, unit182/182 (11971 assertions), integration72/72 (1281 assertions) pass. Final18-scenario network acceptance pending.
- **Second network red**: at925ec3a9 both Provider variants pass every check except the exact selected Provider:17/17 and22/22 post-renewal successes still mix Provider/A and B. The App now passes B correctly; `handleRequestAckByName` checks Controller permission but omits the request's explicit Provider set. Stop the campaign driver after retaining both failures (driver143); the in-flight User control also completes successfully. This partial cohort is not a final18-case gate.
- **ACK fix**: reject decoded ACKs outside a nonempty pending-call Provider set before status hints, ACK metrics or selection; keep empty-list discovery behavior. Complete any tracked decrypt accounting on rejection. A new native case verifies FirstResponding, RandomSelection and AllSelected refuse another Controller-authorized Provider and select the requested one. Rebuild and full final native/network gates pending.
- **ACK native checkpoint**: build exit0 in8m2.858s; six closures pass; unit183/183 (11998 assertions), integration72/72 (1281 assertions) pass. Final18-case network cohort still required.
- **T021 closure**: final `campaign-ack-final/` at clean994018ac passes18/18 scenarios,188/188 scenario assertions and both dedicated User grant gates. Driver and all CLI exits are0; all33 artifact hashes match disk. Provider target/control successes are17/17 +32/32 and22/22 +65/65; User grants10/10 +24/24 and21/21 +60/60. Planned restart/outage role exits-2 are explicitly covered; other role exits0. `final-network-verification.log` and `evidence/provider-online-grant-20260905.md` are authoritative. Earlier failures are not relabeled.

## 2026-09-05 — NAC-ABE compatibility review exposes dependency boundary defects

- **Area**: Spec179 T020, NAC-ABE Experimental compatibility and callback ownership.
- **Symptoms**: two new real CK fan-out tests abort with memory-access violations when success/error callbacks call `clearCache`; late parameter replies replace new state (two failed assertions); current Authority bytes are returned under an unavailable old version (two failed assertions); wrong/empty CP/KP keys decrypt after another key warmed the singleton cache (four failed assertions).
- **Root causes**: application callbacks invalidate the live waiter-map iterator; ParamFetcher fences neither fetch/validation nor retry generations; Authority reflects a requested exact name instead of its actual generation; the inherited crypto cache is indexed by ciphertext alone. The latter predates both reviewed commits but prior tests manually cleared it before negative-key checks.
- **Repair**: detach CK batches before callbacks and check generation between waiters; fence parameter delivery, validation and retries, and commit decoded/name/digest-checked candidates atomically; construct canonical Authority names; bind the crypto cache to a hashed length-delimited scheme/parameters/private-key/ciphertext tuple. Restore the original no-argument ParamFetcher entry and document class-layout rebuild requirements and silent cancellation semantics. Verification is in progress.
- **Evidence**: `results/spec179-nac-compatibility-20260905/red-{reentry-success,reentry-error,params-authority,cache}.log`; durable report `specs/179-request-scoped-confidentiality/evidence/nac-abe-compatibility-review-20260905.md`.
- **Test/tool findings**: first full NAC run passed41/42; the sole failure was an existing lifecycle probe requiring an unset role variable. Default it to the User role while retaining explicit role validation; CTest also needs the fixture directory and a supported report option. An initial class-layout probe omitted ndn-cxx at link time; relink with its pkg-config libraries. A provisional focused run started before final test linking and used the previous test executable, so it is not the final gate.
- **Inspection failure**: `objcopy --dump-section` without an explicit output object rewrote both input ELF files during an installed-prefix test. Stop that test (exit143, `nac-installed-interrupted.log`), relink from unchanged objects and reinstall to restore the original hashes, then repeat installed-prefix execution. Use read-only ELF readers or disposable input copies for future section comparisons; never inspect live libraries with an in-place tool.
- **Dependent build failure**: the mandatory layout rebuild hit GCC9 `internal compiler error: in ggc_set_mark, at ggc-page.c:1547` in system `basic_string.h`, while compiling `HybridMessageCrypto.cpp` for integration-tests (`ndnsf-build.log`, exit1). Retain completed objects and retry once at the same `-j2`; repeated compiler failure requires the already established Clang10/system-binutils fallback. This is not an executed runtime test failure.
- **ABI rebuild finding**: the retry linked successfully in6m35.140s, but object timestamps showed Controller flow and generic API tests still dated15:24–15:27, before the17:22 parameter-header layout change. A successful incremental link is insufficient: invalidate stale objects for the selected framework/tests/three Apps, retain the exact path list, and rebuild again at `-j2`. Unrelated build targets are excluded. No tests from the stale-object link count as acceptance.
- **Compiler fallback**: after invalidating90 selected stale objects, GCC again crashed in `basic_string.h` and produced a non-constant assembler `.size` expression (`ndnsf-build-fresh-objects.log`, exit1). Stop GCC retries. Configure a new `build-clang-spec179-nac-compat` with explicit Clang10, system binutils, the exact NAC prefix and `-j2`; no cached GCC objects enter that build.
- **Strict compiler finding**: Clang rejects the unused `this` capture in the User status-restore validation-error callback (`clang-build.log`). The mirrored Provider callback has the same unused capture. Remove only those captures; retain `-Werror` and the callback body. This is the only NDNSF runtime source change in the dependency review.
- **Lesson**: tested cancellation must include callback reentry and validator latency; cache tests must retain a warm cache for unauthorized callers. Source-compatible calls do not establish binary-layout compatibility.
- **Native checkpoint**: NAC42/42 full cases,20/20 installed-prefix cases; clean Clang NDNSF build exit0 in21m10.471s, unit182/182 (11971 assertions), integration72/72 (1278 assertions), all six target dependency closures pass. Complete16-scenario MiniNDN rerun pending.
- **T020 closure**: fresh16/16 MiniNDN runs completed with CLI exit0,161/161 scenario assertions and both dedicated User grant gates. All33 artifact hashes match disk and every manifest binds clean de1eb508. Planned Provider restart/Controller outage exits-2 are checked by their scenarios. See `campaign-verification.log` under the evidence root. Provider online first grant remains a separate T021 coverage gap.

## 2026-09-05 — NAC dependency test build retained a removed Boost prefix
- **Area**: T019 dependency regression build
- **Symptom**: after correcting a missing test error-header include and parenthesizing a Boost assertion message, test compilation succeeded but linking required absent `/usr/local/lib/libboost_unit_test_framework.so.1.82.0` (`gates/nac-revocation-red-build2.log`).
- **Root cause**: enabling tests reused stale Boost CMake cache entries in the existing exact-prefix build directory.
- **Fix**: unset only `Boost_*`/`boost_*` cache entries, configure `BOOST_ROOT=/usr` and `Boost_NO_BOOST_CMAKE=ON`; verify all resolved Boost libraries point to system1.71 before rebuilding with `-j2`. Expanded dependency tests subsequently passed14 cases/90 assertions.
- **Lesson**: enabling a previously disabled target can reveal stale optional dependency paths even while the shared-library target builds successfully.

## 2026-09-05 — late NAC content callback survives cache invalidation
- **Area**: Spec179 T019, local NAC-ABE Consumer and OpenABE error boundary
- **Symptom**: WAL campaign retry scenario passes all14 business checks, but Provider/A aborts(-6) with `Specified length is invalid` and uncaught `oabe::_OpenABE_ERROR` immediately after epoch3 installation. The process-exit gate correctly rejects it. Driver stopped(exit143); six completed probes retained, five passed. This is incomplete failed evidence.
- **Mechanism reproduced**: Consumer increments `m_cacheGeneration` only in `clearCache`; neither asynchronous content nor CK completion/error checked it. `nac-consumer-revocation-red2.log` fails5/23 assertions: late content reaches crypto with cleared DKEY and produces the same OpenABE error; late CK refills the cache. CP/KP enum conversion fails2/4 separately. GDB on the preserved old binary/library catches the enum in `constructKeyFromBytes -> parseKeyHeader -> importUserKey -> ABESupport::decrypt`, with the caller blocked in `Consumer::onCkeyData -> decryptContent -> SegmentFetcher` (`gates/nac-late-content-gdb-red.log`). This is the controlled reproduction's stack; the original network process has no stack dump.
- **Fix**: generation-fence both fetch stages' completion/error callbacks and normalize OpenABE enum errors into `NacAlgoError`. Unchanged Consumer tests pass23/23 and CP/KP error/recovery passes4/4; expanded dependency14 cases/90 assertions pass. Committed as NAC-ABE `8b462d0`, exact prefix installed and NDNSF resolution verified. Final NDNSF unit182/182, integration72/72 and full MiniNDN16/16 pass; the retry scenario retains14/14 business checks and all five processes exit0. No wire format, permission ownership, or revocation timing changes.
- **Closure evidence**: `specs/179-request-scoped-confidentiality/evidence/online-authorization-audit-20260905.md` and `results/spec179-online-auth-20260905/campaign-nac-final/`. This final cohort also closes the earlier pending online-grant, initial-DKEY admission, callback, namespace fault and PIB regression entries below; their intermediate failures remain historical evidence.
- **Lesson**: clearing maps and rejecting new requests does not cancel callbacks already admitted under old authority; every delayed stage must retain and verify its originating generation.
## 2026-09-05 — shared MiniNDN PIB reader races another role's startup write
- **Area**: Spec179 online-grant fixture, shared campaign PIB
- **Symptom**: the final-readiness late-grant probe had target21/21 but control0/0: User/A exited1 with `Signing certificate ... does not exist` before its first request. The gate correctly failed. Its certificate/key remained present in the retained PIB. Ordinary grant and eight other completed probes are retained; the driver was stopped (exit143) after this failure, allowing the active ninth probe to clean up normally. This is an incomplete failed campaign, not16-scenario acceptance.
- **Root cause evidence**: the fixture used DELETE journals; installed ndn-cxx PIB SELECT paths treat non-ROW, including BUSY, as absent and configure no busy timeout. The added concurrent-reader regression reproduces `database is locked` under the old setup (`gates/pib-concurrency-red.log`). Lock contention is the supported explanation for the transient App failure; that process did not log SQLite's nested return code.
- **Fix**: initialize the campaign-only PIB in WAL mode before any role starts, require the returned mode to be WAL, and record it in the manifest. Existing writer initialization serialization remains. No host PIB, native library, permission rule or deadline changes. Full harness and a fresh network campaign verify the repair.
- **Lesson**: isolated network namespaces do not isolate an intentionally shared SQLite key store; preserve concurrent signing reads as well as serializing initialization writers.

## 2026-09-05 — base RequestMessage overload bypasses shared admission helper
- **Area**: Spec179 initial-DKEY repair verification
- **Symptom**: first repaired build passed the unwrap callback checks but still failed the two publication checks (6/8, `gates/readiness-green.log`).
- **Root cause**: five convenience/Targeted callers used `prepareRequestControllerVersion`, but the base RequestMessage overload entered `startRequestServiceWithRequestId` directly, duplicating only status/version checks. Checking the helper's callers alone missed that separate entry.
- **Fix**: replace the duplicated base-entry checks with the same shared readiness/status helper. The unchanged regression now passes8/8 (`gates/readiness-base-green.log`); all five native targets rebuilt successfully with `-j2` in12m58.644s (`gates/build-base-admission-green.log`). Expanded unit182/182 and integration72/72 passed (`gates/unit-base-final.log`, `gates/integration-base-final.log`); MiniNDN remains pending.
- **Lesson**: trace from the public failing call to the publication boundary; a helper's caller list does not prove every entry uses it.

## 2026-09-05 — permission renewal admits requests before initial DKEY installs
- **Area**: Spec179 asynchronous User startup and hybrid decrypt callbacks
- **Symptom**: normal grant still failed with target11/13 despite status refresh. NAC Consumer debug shows permission renewal at12s coalescing behind the initial DKEY fetch; the stale result is discarded near15s before a replacement installs. ACKs for the first two requests arrive while the Consumer reports no private decryption key, with no application error callback.
- **Root cause**: removing constructor blocking exposed an implicit prerequisite: Request admission checks permission/status but not initial Consumer readiness. Both runtime hybrid decrypt functions move `onError` into the success closure before constructing the unwrap error callback, leaving the latter empty.
- **Fix**: gate real network Request admission on initial Consumer readiness while retaining LocalMock fixture semantics; copy error callbacks into the asynchronous success/unwrap branches and retain synchronous exception reporting. The real-constructor regression reproduced four failed assertions (publication1 instead of0, errors0 instead of1); `gates/readiness-red.log`, exit201. The pre-fix binary is retained. Fixed rebuild/regression is pending.
- **Evidence**: `grant-crypto-diagnostic/user-B.log`; earlier `campaign-grant-final` also retains one control timeout at the grant/status transition. Exact-version rejection is expected during distributed convergence; do not claim instantaneous default-policy convergence or hide that failed row.
- **Lesson**: asynchronous construction must replace former implicit prerequisites with explicit admission checks; moving a shared callback into one branch can silently disable another branch.
- **Probe setting**: normal grant now uses the existing 250ms status refresh knob (four opportunities per 1rps request interval); late grant retains1s. Every failed row remains counted. These measured settings do not establish zero interruption with default status-refresh timing.

## 2026-09-05 — grant control failures were not included in the gate
- **Area**: Spec179 grant-only control and status-convergence evidence
- **Symptom**: normal renewal probe failed with target12/13 and control12/24 successes. The collector exposed control successes only, so the twelve control failures were absent from gate conditions.
- **Root cause**: successful-only control projection lacked a companion failure count. Separately, this grant probe disabled scheduled status refresh: providers learned epoch2 from target traffic while User/A remained epoch1, and the first target request's ACK waited on Provider refresh. Exact-version rejection remains required; a short convergence probe cannot assume all peers instantly discover grant-only changes.
- **Fix**: record every control row and require zero control failures; the new regression fails before the change. Use the existing 1s status refresh knob for the normal grant probe, matching the corrected late-grant case and other bounded revocation tests. This is an explicit experiment/deployment setting, not a claim of instantaneous convergence under production defaults. Retain the 12/24 negative run; recheck earlier late-grant raw rows against the stronger control rule. Final normal grant rerun pending.
- **Ref**: `campaign-renewal/grant-only-advance`; `gates/harness-control-red.log`; T018.
- **Lesson**: key reuse and status-version convergence are separate conditions; count all control outcomes, not only successful ones.

## 2026-09-05 — asynchronous startup exposes three MiniNDN timing assumptions
- **Area**: Spec179 grant and offline-rejoin probes (T018)
- **Symptom**: rebuilt full campaign finished 13/16, exit 1. Normal grant had no renewal and zero granted requests; late grant had 21/21 successes but no exhausted permission retries; offline User installed epoch 2 before reaching epoch 3.
- **Root cause**: old constructor blocking implicitly deferred permission discovery until grant. Once constructors return, an empty permission response completes normally, so absence of a grant does not force transport retry exhaustion. Fixed SIGCONT timing relied on old startup skew and could precede the actual epoch-3 revoke. The late control also needed scheduled status renewal for its 60-second window.
- **Fix**: both grant probes use explicit App refetch; late probe drops outgoing UDP only inside the isolated user-b namespace until observed final permission timeout, then removes the exact rule in finally. A namespace guard refuses host execution. Keep control statuses renewed through existing knobs. Offline SIGCONT waits for the actual revoke marker. No failed rows or ordering checks are removed. Three focused MiniNDN reruns pending.
- **Ref**: `results/spec179-online-auth-20260905/campaign-final/`; `permission-startup-loss.json` in the corrected late run records both fault boundaries. Native binaries and libraries are unchanged for the rerun.
- **Lesson**: test prerequisites must be observed, not inferred from constructor latency, empty responses, or estimated mutation deadlines.

## 2026-09-05 — stream retry timing assertion fails alongside compilation
- **Area**: Spec179 final integration verification / shared-host load
- **Symptom**: `NormalStreamRetriesOneSuppressedEventFromProviderIms` completed successfully but recorded two retries instead of exactly one; the expanded gate returned 201 (70/71 cases, 1269/1270 assertions).
- **Root cause**: the test uses a 100ms Interest lifetime; concurrent `-j2` App compilation is a plausible scheduling cause, not yet established by a controlled load experiment. No source change to the stream implementation occurred in this repair.
- **Fix**: retained `gates/integration-final.log`, finished compilation and reran without compilation load. The isolated case passed immediately (12/12 assertions, exit 0); full isolated integration gate passed 71/71 cases and 1270/1270 assertions (`gates/integration-final-isolated.log`). No retry assertion or timeout was weakened.
- **Lesson**: independent binaries avoid link races but do not isolate timing-sensitive tests from shared CPU pressure. Run the final timing gate without compilation.

## 2026-09-05 — unprovisioned runtime cannot reach online permission renewal
- **Area**: Spec179 User/Provider construction and online grant (T018)
- **Symptom**: late-grant MiniNDN probe failed the App_User readiness deadline; no permission fetch or App refetch marker appeared, only repeated Waiting for decryption key lines. The original first-grant scenario began the App only after grant unlocked construction.
- **Root cause**: both real constructors synchronously pump their Face until Consumer has a DKEY. An identity with no grant cannot finish construction to call the App-owned permission API. This corrects the earlier startup-delay hypothesis: the relevant delay was the DKEY gate, not slow RSA initialization.
- **Fix**: begin the existing asynchronous Consumer fetch and return with an explicit bootstrap-pending marker. Keep all permission/status/key checks. Timed real User/Provider constructor coverage passes 8/8 assertions with zero unauthorized publication/execution; final late-grant network rerun remains pending.
- **Ref**: campaign/grant-after-permission-exhaustion; ServiceUser/ServiceProvider constructors; UnprovisionedRuntimesConstructAndRemainUnauthorized.
- **Lesson**: an application-owned recovery API is unusable if construction blocks waiting for the condition that API must recover.

## 2026-09-05 — one-second benchmark drain truncates delayed valid responses
- **Area**: Spec179 MiniNDN workload shutdown
- **Symptom**: corrected-duration inflight-revocation run reported two unsuccessful unaffected-user requests near workload end, despite ten successful post-revoke requests.
- **Root cause**: the launcher allowed only one second of drain for a three-second Provider delay and five-second request timeout. Open-loop finalization emitted incomplete rows before valid in-flight work could finish.
- **Fix**: drain six seconds (five-second request timeout plus margin), use at least 35-second role windows for the standard scenarios, retain all failures and rerun. No success filter is added.
- **Ref**: campaign/inflight-revocation; App_User drainDeadline; T018.
- **Lesson**: measured-window completion and process survival must include the entire request drain budget.

## 2026-09-05 — failed withdrawal bypassed by grant or same-target retry
- **Area**: Spec179 Controller pending ABE rotation
- **Symptom**: the new real Controller regression produced 10 failed assertions: same-target retry left the old ABE pair; grant removed the revocation during injected rotation failure; direct recovery produced an equal-version conflicting status rejected by RevocationState.
- **Root cause**: duplicate-target return preceded reconciliation; grant never checked pending rotation; reconciliation reused a ControllerVersion whose old parameter identity could already be published.
- **Fix**: reconcile before duplicate handling and grant mutation, persist a newer epoch before recovery crypto work, return successful completion for the pending same-target retry, and retain ordinary completed-duplicate no-op behavior. One-shot injection and a repeated App revoke support the matching MiniNDN scenario.
- **Ref**: T017; PendingRotationFencesGrantAndPreservesImmutableStatus; `results/spec179-online-auth-20260905/gates/controller-red.log` (exit 201, 10 failed assertions) and `controller-green.log` (exit 0, 36/36 assertions, including old/replacement DKEY decryption). MiniNDN `campaign/revocation-rotation-failure-retry`: 14/14 checks pass, epoch 2 -> 3, affected denial throughout, 16/16 unaffected post-recovery calls succeed.
- **Lesson**: failure recovery is an authorization mutation too; test every public entry and preserve already published immutable status identities.

## 2026-09-05 — late-grant probe initially missed its timing contract
- **Area**: Spec179 MiniNDN T018 probe
- **Symptom**: first late-grant run completed but gate failed (exit 4): user/B started after the Controller grant, no startup timeout exhaustion occurred, and the result exporter omitted the new scenario's grant evidence.
- **Root cause**: startup outlasted the 12-second grant offset; the later probe identified the constructor DKEY wait (see the entry above), superseding the initial RSA-delay hypothesis. One scenario-name equality remained in the evidence return despite sharing the grant collector.
- **Fix**: grant evidence now follows the grantOnlyAdvance configuration for both scenarios; added an exporter regression. Increased grant/renewal/workload windows and require measured exhaustion < grant < refetch <= first invocation plus post-refetch unaffected successes. First run retained as a failed timing probe.
- **Ref**: results/spec179-online-auth-20260905/late-grant-first; 12 launcher tests pass. Corrected network rerun pending.
- **Lesson**: launch offsets are assumptions; acceptance must verify event ordering from observed timestamps.

## 2026-09-05 — MiniNDN open-loop milliseconds interpreted as seconds
- **Area**: Spec179 launcher workload lifetime
- **Symptom**: a run configured for 16 seconds kept enqueueing until the process lifetime killed it; late-grant-first user/A enqueued for about 51 seconds despite the nominal workload/count.
- **Root cause**: requestDurationMs was passed directly to App_User --duration, which uses std::chrono::seconds; --count applies to closed-loop mode and does not cap this open-loop workload. Teardown could therefore truncate in-flight requests, previously hidden by the grant collector.
- **Fix**: round milliseconds up to integer seconds at the App_User command boundary. The final late-grant probe uses 60-second workloads for both users, spanning explicit renewal at 40 seconds after App startup; the fault/retry scenario explicitly spans both mutation events and drains before process shutdown.
- **Ref**: examples/App_User.cpp openLoopDurationSeconds and measurementStopAt; run_request_scoped_confidentiality.py user_command; T018.
- **Lesson**: verify units at the actual CLI consumer and distinguish open-loop duration from closed-loop count.

## 2026-09-05 — online authorization audit detects censored MiniNDN failures
- **Area**: Spec179 MiniNDN evidence and exit status
- **Symptom**: a success plus a failed request while providers were alive was counted as one successful row; a completed run with gatePassed=false returned exit code 0.
- **Root cause**: the grant collector used the earliest provider log timestamp as a termination cutoff and dropped failure rows; main checked process completion alone.
- **Fix**: retain all terminal rows, allow bootstrap/workload/drain in the grant scenario lifetime, and require gatePassed=true for exit 0. Two regression cases reproduced both defects before the fix; the 11-case launcher suite passed afterward.
- **Ref**: tests/minindn/test_request_scoped_confidentiality.py; /tmp/spec179-online-auth-harness-red.log and harness-green.log; T018.
- **Lesson**: process completion is not a security gate; never infer teardown from a first log or silently discard negative evidence.
- **Real-run confirmation**: reprocessing `results/spec179-online-auth-20260905/baseline-grant` retained 16 requests with 14 successes and 2 timeouts. The old collector had reported 14/14. The campaign driver now propagates failed gates, uses fresh output directories, and refuses to overwrite retained scenarios; each new manifest records revision, working diff and executable/library hashes.

## 2026-09-05 — online authorization audit preflight and test authoring corrections
- **Area**: Context Mode and Controller regression fixture
- **Symptom**: active authority hashes were stale; project query guard rejected a low-entropy identifier and then an identifier absent from its query; a new C++ regression did not compile.
- **Root cause**: prior Spec edits were not indexed; malformed guard arguments; makeServiceRevocation takes const char* rather than std::string.
- **Fix**: reindexed canonical authority documents, verified active health, corrected and reran the guarded project query; passed the temporary URI through c_str for the immediate copying helper call.
- **Lesson**: use file-backed checkpoints after retrieval failures and verify fixture signatures before writing a regression. An initially rejected query is not accepted authority.

Append-only engineering failure record. Rule (AGENTS.md): every failure that
costs non-trivial debugging MUST be appended here **in the same checkpoint
commit that fixes or records it**. New tasks MUST read the recent entries as
part of task context. Format per entry:

```text
## <date> — <one-line symptom>
- **Area**: <spec or module>
- **Symptom**: <what was observed>
- **Root cause**: <why>
- **Fix**: <what changed / workaround>
- **Ref**: <commit, evidence file, or script>
- **Lesson**: <one line to carry forward>
```

## 2026-09-05 — git index duplicate entries wrote a corrupted tree
- **Area**: tooling/git
- **Symptom**: `git add -A` with a pathspec containing a comma staged
  duplicate index entries; the resulting commit tree had `duplicateEntries`
  + `treeNotSorted` (`git fsck` errors), and a rename-detection warning
  "duplicate destination".
- **Root cause**: comma pathspec left the index with unordered/duplicate
  stage entries.
- **Fix**: `rm .git/index && git reset --mixed <last-good>` rebuilt the
  index; re-staged with explicit paths; `git prune --expire=now` dropped
  the bad commit.
- **Ref**: NDNSF commits `32b1fc23` (re-created) replacing the bad
  `8fc879ce`.
- **Lesson**: never use `git add -A` with comma/odd pathspecs; verify
  `git ls-files | sort | uniq -d` is empty before committing.

## 2026-09-05 — stale campaign-summary.tsv contradicted final MiniNDN results
- **Area**: Spec179 evidence
- **Symptom**: `results/spec179-minindn/campaign-summary.tsv` showed many
  scenarios `gatePassed=False` while per-scenario `result.json` said true.
- **Root cause**: the TSV predated the final campaign runs (13:08 vs runs
  15:47–17:18) and was never regenerated.
- **Fix**: regenerated from the final `result.json` files (14/14
  `gatePassed=True`).
- **Ref**: Spec179 `evidence/post-implementation-audit.md` R179-A4.
- **Lesson**: derived summary artifacts must carry a timestamp and be
  regenerated, or deleted, after the runs they summarize.

## 2026-09-04 — MiniNDN campaign caught two admission defects
- **Area**: Spec179 runtime
- **Symptom**: S9 — `RequestServiceTargeted` issued versionless requests;
  S10 — `requestServiceStreamingBytes` discarded the admission result so a
  denied stream start logged STARTED.
- **Root cause**: missing version binding on the Targeted request path;
  ignored revocation admission result on the stream start path.
- **Fix**: fixed in `ServiceUser.cpp`; rebuilt; genuine campaign rerun
  green.
- **Ref**: `evidence/minindn-campaign-20260904.md`, NDNSF commit `e7ea0a74`.
- **Lesson**: the cross-process campaign is the admission-boundary oracle;
  component tests did not catch either defect.

## 2026-09-03 — OpenABE mixed-generation decrypt returns garbage
- **Area**: NAC-ABE/OpenABE crypto
- **Symptom**: decrypting new-generation ciphertext with a retained old
  DKEY could return garbage plaintext instead of throwing.
- **Root cause**: OpenABE generation mismatch does not always fail loudly.
- **Fix**: Spec179 test assertions use `decryptFailsClosed` (throw **or**
  recovery failure both accepted); RV-U20 mixed-generation matrix with
  fresh ciphertext per case (the ABESupport singleton CK cache would
  otherwise mask the mismatch).
- **Ref**: Spec179 `evidence/runtime-revocation-lifecycle-20260904.md`.
- **Lesson**: crypto-negative assertions must accept "recovered garbage"
  as failure; never reuse a successfully-decrypted ciphertext in a
  generation-mismatch case.

## 2026-09-03 — NAC-ABE stale DKEY after grant-only policy replacement
- **Area**: NAC-ABE dependency
- **Symptom**: target refresh could receive the previous complete DKEY.
- **Root cause**: DKEY segments published with `FreshnessPeriod=4s`; the
  unversioned `MustBeFresh` discovery Interest then hit a still-fresh
  Content Store copy of the old policy.
- **Fix**: DKEY segments now publish with `FreshnessPeriod=0`; exact
  versioned segment names remain retrievable.
- **Ref**: NAC-ABE `Experimental` branch commit `b1c9c4f` (not pushed).
- **Lesson**: any in-place policy replacement needs freshness discipline
  on unversioned discovery names.

## 2026-09-03 — versioned exact public-params Interest could never match
- **Area**: NAC-ABE dependency
- **Symptom**: after status installation,
  `refreshPublicParameters` with the exact
  `/PUBLIC-PARAMS/<ABE-TYPE>/v=<version>` name (CanBePrefix=false) timed
  out repeatedly.
- **Root cause**: `AttributeAuthority::onPublicParamsRequest`
  unconditionally appended `<ABE-TYPE>` + version to the Interest name,
  producing a Data name that can never satisfy the exact request.
- **Fix**: detect an already-versioned name and do not append again;
  ParamFetcher binds expected name/digest.
- **Ref**: NAC-ABE `Experimental` commit `b1c9c4f`.
- **Lesson**: producer-side name derivation must mirror every Interest
  shape the consumer may legally send.

## 2026-09-02/04 — build and test-environment traps (Spec179 baseline)
- **Area**: build/tests
- **Symptom** (three independent traps):
  1. GCC 9 ICE on `data-enc-dec.cpp` — NAC-ABE must be built with
     `clang++-10`.
  2. Two concurrent waf builds in different out dirs conflict on the
     shared lock and one is killed silently.
  3. After a full-suite SIGSEGV, Boost.Test keeps running and the
     residual process disturbs later timing runs — kill residuals before
     re-running.
- **Fix**: documented build recipe (clang++-10, single build at a time,
     kill-then-retest).
- **Ref**: Spec179 `evidence/restore-fixes-20260904.md`,
     `evidence/regression-red-green-20260904.md`.
- **Lesson**: environment traps must be recorded next to the build
  recipe, not rediscovered per session.

## 2026-09-02 — DummyClientFace hangs and LocalMock DKEY reattach
- **Area**: tests
- **Symptom**: `processEvents` blocked forever on a fully idle face;
  pump-driven LocalMock members could not verify DKEY segments.
- **Root cause**: deferred DKEY reattach had no bound when the face went
  idle.
- **Fix**: bounded retry (250 ms × 20) for deferred DKEY reattach;
  request-pump fixture extended to pump the AA face (Spec179 remounts).
- **Ref**: Spec179 baseline fixes in NDNSF commit `e7ea0a74`.
- **Lesson**: every deferred async retry needs a bounded schedule or an
  idle-face test can deadlock the whole suite.

## 2026-09-02 — SegmentFetcher infinite fetch on discovery Data
- **Area**: Core `ServiceProvider::replyFromIMS`
- **Symptom**: SegmentFetcher kept requesting segments until timeout.
- **Root cause**: discovery Data served from IMS lacked `FinalBlockId`.
- **Fix**: forward to the last contiguous IMS segment and set
  `FinalBlockId`.
- **Ref**: Spec179 baseline fixes.
- **Lesson**: any segmented Data served to a SegmentFetcher must carry a
  terminal marker or the fetch is unbounded.

## 2026-09-07 — Spec182 T006-D: worker child catch mislabels every chain rejection as DI_NATIVE_ONNX_WORKER_INTERNAL
- **Area**: spec182 T006-C/T006-D native worker wire protocol (frozen in
  T006-C); child side of `runNativeOnnxAssemblyWorkerMain`.
- **Symptom**: focused `Spec182OnnxActivation` case
  `ActivationRejectsCertifiedGraphPoisonThroughWorker` failed while T006-D
  verified the real worker binary: the frozen `reject-identity-digest`
  vector (poisoned `recipe.graphDigest`) surfaced from the parent transport
  as `DI_NATIVE_ONNX_WORKER_INTERNAL` instead of the required chain
  rejection code `DI_NATIVE_ONNX_RECIPE`.
- **Root cause**: off-by-one in the child catch of
  `NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxAssemblyWorker.cpp`
  (shipped in the T006-C commit). The guard was
  `what.compare(0, 14, "DI_NATIVE_ONNX_") == 0`, but the family literal is
  15 bytes; the three-argument `compare()` treats the whole literal as the
  right side, so it compared a 14-byte prefix of `what` against the whole
  15-byte literal — never equal. Every chain rejection (`fail(code)` →
  `"DI_NATIVE_ONNX_" + code`, S1-S7 of the certified chain) was therefore
  reclassified as `DI_NATIVE_ONNX_WORKER_INTERNAL`, and the parent
  transport (which propagates the worker error code verbatim when it
  carries the `DI_NATIVE_ONNX_` family prefix) relayed the wrong reason.
  The frozen suites had never pushed a reject row through the real worker
  child, so the defect only surfaced during T006-D activation coverage.
- **Fix**: bound 14 → 15 (`what.compare(0, 15, "DI_NATIVE_ONNX_") == 0`),
  restoring verbatim family-code propagation. A new frozen-lock case
  `Spec182OnnxWorkerProtocol/SubprocessChainRejectionPropagatesItsOwnCode`
  spawns the real worker with the frozen reject row and asserts the parent
  sees exactly `DI_NATIVE_ONNX_RECIPE`; registered in
  `tests/fixtures/spec182/case-manifest.json`.
- **Ref**: run dirs retained under `.codex-tmp/` (`t006d-*` logs);
  manifests `tests/fixtures/spec182/case-manifest.json`.
- **Lesson**: a protocol invariant ("reason-family code must arrive
  verbatim at the caller") needs at least one end-to-end lock that drives
  a real subprocess with a frozen reject vector; unit mocks of the child
  catch could not expose the string-compare bug.

## 2026-09-07 — Spec182 T007-A: wscript helper insertion broke _pin_compiler_toolchain; boost 1.71 cannot print std::vector
- **Area**: spec182 T007-A static Rust tokenizer link; root `wscript` and
  `tests/unit-tests/di-native-tokenizer.t.cpp`.
- **Symptom**: (1) configure failed rc=2 with `NameError: name 'tools' is
  not defined` at wscript ~line 152 — the `_ensure_tokenizer_bridge` helper
  had been inserted in the middle of `_pin_compiler_toolchain`, and that
  function's tail statements (`conf.env.NDNSF_LINKER`, the 'Closed C++
  toolchain' msg) dangled at 4-space indent, becoming the helper's last
  statements. (2) The compile fix then revealed a duplicate
  `_pin_compiler_toolchain` def (original tailless copy plus a reconstructed
  complete copy) — Python shadowing made it work but left ~25 lines of dead
  code. (3) `unit-tests` compile failed on `BOOST_REQUIRE_EQUAL(encode(), ids)`:
  boost 1.71's `print_helper` has no `operator<<` for `std::vector<long>`.
- **Root cause**: (1) `Edit` with an old_string ending mid-function appended
  the new helper inside the old function body; indentation kept the tail
  inside the helper. (3) boost 1.71 test-tools cannot stream a vector; the
  assertion is fine at runtime but does not compile.
- **Fix**: (1) re-emitted the helper as a complete module-level function and
  restored `_pin_compiler_toolchain` with its own tail; (2) deleted the dead
  original copy, keeping one documented def; configure rc=0 with 'Pinned
  Rust tokenizer staticlib' resolved. (3) switched vector equality to
  elementwise `BOOST_REQUIRE_EQUAL_COLLECTIONS` (three sites:
  `compareVectorCase`, owner-reuse roundtrip, concurrency baseline check).
- **Ref**: run dirs retained under `.codex-tmp/` (`t007-configure-r2.log`,
  `t007-build-r3.log`, `t007-full-regression.log`); cargo PATH lesson: the
  pinned rustc must be on PATH (`rust-prefix/bin`) or `cargo build` dies
  with "could not execute process `rustc -vV`".
- **Addendum（同卡）**: an unfiltered full run (`./build-nac182/unit-tests`,
  no exclusion) segfaulted rc=139 inside the known environment-dependent
  stream-facade family (`PredictiveProviderExactWireValidationAndAtomicFlush`,
  stream-facade.t.cpp:270 last checkpoint); negative preserved, same family
  every spec182 card excludes since T006-B/C/D. The card regression gate ran
  with `--run_test='!StreamFacade'` → 859 cases, No errors detected.
- **Lesson**: insert a new top-level `def` only with an old_string that ends
  at a module-level boundary (blank-line pair); after any structural waf
  edit, re-run configure before building. Assert `std::vector` equality in
  boost 1.71 with `EQUAL_COLLECTIONS`, never `REQUIRE_EQUAL`.

## 2026-09-07 — Spec182 T007-B: stale Rust staticlib silently kept old ABI; python codec vs Rust-std surrogate prefix divergence
- **Area**: spec182 T007-B stable-prefix decode; `wscript`
  `_ensure_tokenizer_bridge`, `tests/unit-tests/distributed-inference-tokenizer.t.cpp`.
- **Symptom**: after rewriting `tokenizer-bridge/src/lib.rs`, the focused
  `Spec182TokenizerStable/*` run still failed with the *old* behavior
  (ByteLevel prefix `[a != a�]`, reject-Fuse stable calls not throwing).
  A reconfigure rebuilt the archive (`13:03:24`) but `./waf build` finished
  in 17 s having relinked nothing: both `libndnsf-distributed-inference.so`
  (12:59) and `unit-tests` (13:01) predated the new archive, and a second
  failure signature then appeared (surrogate cut `[�� != ]`).
- **Root cause**: (1) the archive lives *outside* the build dir
  (`.codex-tmp/spec182-t001-dependencies/tokenizer-bridge-target/...`);
  waf links it by path and does not signature-track external STLIB files, so
  a changed archive alone never dirties the link task — and waf is
  content-hash based, so `touch`ing a source does not help either.
  (2) Authoring-proxy divergence: the frozen ByteLevel row expectations came
  from python's incremental UTF-8 codec (`errors=replace`), which *defers*
  a 3-byte-lead decision until its third byte; Rust std rejects `ED A0`
  eagerly (second byte must be 80..9F), so the surrogate row's mid-prefix
  cuts diverged (`""` vs `"��"`).
- **Fix**: (1) delete `build-nac182/libndnsf-distributed-inference.so` and
  `build-nac182/unit-tests`, then rebuild — outputs missing forces the link
  task to rerun against the new archive (verify with
  `ls --time-style` after every T007-class lib.rs change). (2) rewrote
  `author-stable-vectors.py`'s per-cut model as an explicit mirror of
  `std::str::from_utf8` error attribution (tight E0/ED/F0/F4 second-byte
  ranges, continuation consumption, trailing-incomplete `None`); HF decode
  stays the independent cross-check at full length for every row.
- **Ref**: ABI probes retained at `/tmp/t007b-abi-probe/` (probe.cpp,
  probe2.cpp) and `/tmp/t007b-surrogate-check/` (std-semantics micro
  checks); frozen vectors regenerated, whole-file sha256
  `a80597b3c96833a61a4dd22606b014715f62e2111e0bd7b3325cc97665bc6254`;
  fixture tokenizer shas unchanged.
- **Lesson**: after any `tokenizer-bridge/src/*.rs` change, delete the
  `build-nac182` `.so`/`unit-tests` link products (or run a second
  configure + build and verify mtimes) before trusting a test run; frozen
  per-prefix expectations for ByteLevel must be authored with Rust-std
  utf-8 semantics, not python codec semantics.

## 2026-09-08 — Spec182 T016 preflight: MiniNDN node context unavailable

- **Area**: T016-A local qualification preflight;
  `Experiments/NDNSF_DI_NativeClosure_Minindn.py` campaign owner.
- **Command**: `python3 Experiments/NDNSF_DI_NativeClosure_Minindn.py
  --manifest .codex-tmp/spec182-t016-r2/manifest.json
  --output .codex-tmp/spec182-t016-r2/result` with `campaignCase=I01`.
- **Observed boundary**: exit `2`, result status `UNQUALIFIED`, reason
  `MININDN_NODE_CONTEXT_NOT_PROVIDED`; `/usr/bin/bwrap`, `strace` and `nsenter`
  are installed, but `/run/nfd/nfd.sock` is absent and no real MiniNDN
  `node/netns` metadata was supplied.
- **Interpretation**: this is a harness/environment preflight boundary, not an
  I01 protocol result and not a native no-Python PASS/FAIL. No business process,
  namespace or network request was started.
- **Raw evidence**: `.codex-tmp/spec182-t016-r2/` (manifest, stdout/stderr and
  persisted result.json). Keep this run immutable; a later retry must use a new
  run directory after node/NFD context is available.
- **Next step**: provide the externally owned MiniNDN node/netns/socket context,
  then rerun the complete T016 matrix with a fresh raw run directory.

## 2026-09-08 — Spec182 R6-B7: current binary drops one legacy D2b Selection before provider1 callback

- **Area**: legacy Spec170 `DATA_V1` compatibility path exercised by the
  current Spec182-linked `integration-tests` binary;
  `Spec170NdnsfDiCoreFlow/ProductionIngressRunsD2bSelectionIntoSvsDataV1`.
- **Symptom**: current `build-nac182/integration-tests` returns rc=201. ACK
  collection and collaboration plan commit pass, and provider0 receives,
  decrypts, queues, and completes its Selection. User trace records both
  provider1 and provider0 Selection publications, but provider1 records no
  Selection callback; `provider0Published`, `provider1HandlerCalled`, and the
  bounded DATA_V1 fetch assertion fail. The older `build-system-j2` binary
  passes the isolated selector.
- **First boundary**: after User `PublishServiceSelectionMessageV2` publication
  (both `SVS_PUBLISH_DONE` entries are present) and before provider1's
  `ServiceProvider::handleServiceSelectionMessage` callback. ACK, plan, and
  provider0 execution are not the failing boundary.
- **Attempts**: isolated run with User/Provider TRACE; repeat with
  `NDNSF_HANDLER_THREADS=0`. The handler-thread override did not change the
  failure. An exploratory full `-j4` rebuild was interrupted after `vmstat 1`
  showed sustained swap-in/out; no build result from that attempt is used as
  validation. Raw immutable logs are under
  `.codex-tmp/spec182-r6-b7-legacy-d2b-20260908/`.
- **Interpretation**: preserve this as a current-source compatibility/runtime
  boundary. Do not change `ServiceProvider::isFresh` or claim a Spec182
  qualification result until a focused probe identifies the provider1
  receipt/newness ordering and a named regression selector is added.
- **Next step**: inspect the exact SVS delivery/newness interleaving, then either
  make a bounded compatibility repair with an independent selector or retain
  the failure as a T013-B migration prerequisite. See
  `specs/182-native-di-python-bindings/evidence/r6-b7-legacy-d2b-regression-20260908.md`.

## Proposal authorization PPTX build-path preflight — 2026-09-08

生成器拒绝末级名称不以 `ndnsf-` 开头的临时目录，未覆盖交付物。
首个失败边界为目录安全检查，不是 slides 内容错误。
原始日志：`.codex-tmp/proposal-authorization-20260908-docs/logs/pptx-build.log`。
采用新的合规目录后生成通过，1944/1944 文本 spans 分配通过；详见
`docs/PAPER/proposal-defense/authorization-revision.md`。前述正文引用失败也已通过隔离重建解决。

## 2026-09-09 — Spec182 R7-B2 alternate replacement boundaries

R7-B2 首次批次构建在 `tests/integration-tests/ndnsf-di-core-flow.t.cpp:952` 触发
helper brace/syntax cascade，首个边界是 C++ 编译器解析失败；原始输出保留在
`.codex-tmp/spec182-r7-b2-replacement-map-20260909/build.log`，修复后同一 source
closure 构建通过。

随后双 Provider alternate selector 首次运行到 recovery Selection，但旧的 V2 parser
把 `/NDNSF/DI/REQUEST/1/recovery/2` 截短为 `/NDNSF/DI/REQUEST/1/recovery`，在
provider1 callback 前以 `SELECTION_NO_PENDING` 暴露。首个运行边界不是 Provider 执行或
checkpoint 提交；完整 trace 保留在
`.codex-tmp/spec182-r7-b2-replacement-map-20260909-r4/integration-alternate-trace2.log`。
parser 修复后增加结构化 recovery、结构化 decision 和 legacy `request-1/3` 回归，全部
通过。最终批次结果与未观测的跨进程/T016边界见
[R7-B2 evidence](../specs/182-native-di-python-bindings/evidence/r7-b2-alternate-provider-replacement-20260909.md)。

## 2026-09-09 — Spec182 R6-B9: bounded repair passes D2b but broad suite exposes later D2h crash

- **Area**: current-source legacy Spec170 D2b Selection freshness and `DATA_V1` ingress;
  `ServiceProvider::isFresh`.
- **First boundary and repair**: R6-B7's trace showed provider0 sequence 4 arriving before
  provider1 sequence 3. The old single producer/session frontier discarded the unseen provider1
  publication before its callback. The bounded repair keeps old-session rejection and same-name
  duplicate fencing, while storing sequence frontiers per publication name under `svs_mutex`.
- **Focused result**: current `integration-tests` was rebuilt with system-first `-j4` (118/118,
  exit 0, Waf 1m37.712s). The five D2b selectors and the named
  `ProductionNativeHandlersRunD2h212ToCompleteOracleResponse` selector each exited 0 in isolated
  runs; raw logs are under `.codex-tmp/spec182-r6-b9/`.
- **Remaining failure boundary**: the first unfiltered `Spec170NdnsfDiCoreFlow/*` attempt reached
  the D2h production case with a missing response/role and `double free or corruption`; the same
  boundary reproduced in `2/20` isolated D2h212 repeats. Two subsequent fresh unfiltered runs and
  all `50/50` D2b repeats exited 0, so this remains an intermittent independent runtime/test
  observation rather than an attributed D2b regression.
- **Interpretation**: R6-B9 is `CLOSED_FOR_VALIDATION` only for the local D2b freshness behavior;
  T013-B/T013-C, cross-process compatibility and T016 qualification remain `PARTIAL`/open.
  See [R6-B9 evidence](../specs/182-native-di-python-bindings/evidence/r6-b9-legacy-d2b-freshness-20260909.md).

### 2026-09-09 — R6-B9 follow-up stability observation

The historical D2h212 boundary remains preserved in the R6-B9 run directory (`2/20` failures
with callback/role loss and `double free or corruption`). A later fresh-process rerun of
`ProductionNativeHandlersRunD2h212ToCompleteOracleResponse`, with dependency and NDNSF tracing
enabled, passed `20/20` without timeout; logs are under
`.codex-tmp/spec182-r6-b9/d2h212-dep-repeat20/`. This narrows current reproducibility but does
not explain the historical interleaving, prove cross-process behavior, or close T016.

### 2026-09-09 — R9-B1 D2h212 selection-status UAF

The longer D2h212 sample reproduced `SIGABRT`/`double free or corruption`. An ASAN allocator
preload identified the first invalid access as a heap-use-after-free in
`ServiceProvider::reportSelectionOperationStatus`: concurrent Provider workers appended to the
same `memberStatuses` vector while another worker wrote an element from storage invalidated by
reallocation. The status map was also read by the Face query path without a snapshot lock. The
repair added `m_selectionExecutionStatusMutex` around report/update/get, without changing the
status wire or state-transition contract.

After the repair, the concurrent unit selector passed, D2h212 passed `50/50` fresh processes,
and an ASAN-preload follow-up passed `20/20` with allocator type-size mismatch diagnostics
disabled (the uninstrumented SVSPubSub dependency otherwise reports a non-product size warning).
The original R6-B9 logs remain preserved; cross-process status publication and T016 qualification
are still unobserved.

## 2026-09-09 — Spec182 R10-B5 Boost.Test filter setup miss

- **Area**: R10-B5 Provider REPO_REF execution selector; regression command boundary.
- **First boundary**: the command combining three `--run_test` values with commas returned
  Boost.Test setup code `200` (`no test cases matching filter or all test cases were disabled`)
  before entering any test case. No Provider, network, or native execution started.
- **Interpretation**: this is a test-command syntax/harness miss, not a product failure. The
  corrected suite selector `Spec170NativePostSelection` entered all five cases and passed,
  including the new REPO_REF case.
- **Raw evidence**: `.codex-tmp/spec182-r10-b5/invalid-filter.log` and
  `.codex-tmp/spec182-r10-b5/regression-suite.log`; keep both immutable with the build and
  `vmstat` logs in the same run directory.
- **Next step**: use suite selectors or verified Boost.Test filter syntax for this target; retain
  the failed command in the batch retrospective as a `runtime/test` setup miss.

## 2026-09-09 — Spec182 R10-B6 missing REPO_REF negative exceeded local pump boundary

- **Area**: R10-B6 Provider REPO_REF fail-closed negative; missing encrypted input object.
- **First boundary**: the production handler entered and did not enter the runner, but the
  missing-object fetch did not reach `CollaborationContext::fail` before the fixture's fixed
  200-round (about 3 s) pump ended. The assertions for `statusFailed` and the failure reason
  therefore failed; no successful response was observed. The size-mismatch and malformed-envelope
  cases in the same suite passed.
- **Interpretation**: this is an observed fetch-timeout/test-harness boundary, not a Provider
  success. `ServiceProvider::fetchAndDecryptLargeData` uses the shared
  `NDNSF_REQUEST_LARGE_FETCH_TIMEOUT_MS` budget (default 30 s) and may try the legacy fallback,
  while the helper pump stops earlier. The missing-object result remains unqualified until the
  test uses a deterministic parser-level missing-reference case or an explicitly bounded fetch
  budget.
- **Raw evidence**: `.codex-tmp/spec182-r10-b6/suite-missing-failure.log`, `build.log`, and
  `vmstat.log`.
- **Next step**: repair the negative fixture so its missing-reference boundary is deterministic,
  then rerun the complete `Spec170NativePostSelection` suite before closing R10-B6.

## 2026-09-09 — Spec182 R10-B38 T016 runtime context recheck remains unqualified

- **Area**: T016-A MiniNDN qualification owner preflight after R10-B37.
- **First boundaries**: a fresh default run in `.codex-tmp/spec182-t016-r6/` returned exit `2`,
  `UNQUALIFIED/MININDN_NODE_CONTEXT_NOT_PROVIDED` before business startup. A separate fresh
  `--execute-owner` run in `.codex-tmp/spec182-t016-r7/` returned exit `2`,
  `UNQUALIFIED/MININDN_REQUIRES_ROOT` because the current process is UID 1000.
- **Observed host state**: `/run/nfd/nfd.sock` exists and a system `nfd` process is running, but
  no MiniNDN requester/provider node PID/starttime, network namespace, per-node NFD socket or peer
  metadata is available. The socket alone does not satisfy the owner contract.
- **Interpretation**: these are campaign preflight/environment boundaries, not protocol PASS/FAIL
  and not no-Python evidence. No requester/provider business process, namespace, network request or
  product build was started.
- **Raw evidence**: `.codex-tmp/spec182-t016-r6/` and `.codex-tmp/spec182-t016-r7/`; keep both
  immutable and do not overwrite the earlier R6-B4 preflight.
- **Next step**: provide a root MiniNDN owner context with identity-bound namespaces, PID starttimes,
  independent NFD sockets and peer metadata, then run the complete I01–I08/PO matrix in a new run
  directory.

## 2026-09-09 — Spec182 R10-B40 PO-001 raw manifest preflight retries

- **Area**: T016-A PO-001 owner/runner execution after root owner access was available.
- **First boundaries**: fresh `.codex-tmp/spec182-t016-r10/` reached the canonical runner but stopped
  at `PreflightError: process role is invalid`; fresh `.codex-tmp/spec182-t016-r11/` then stopped at
  `PreflightError: artifact digest mismatch` for the existing `integration-tests` executable.
- **Interpretation**: both failures were runner-manifest identity/configuration boundaries, not native
  protocol results. The old manifest omitted the required `process.role` and carried a stale binary
  digest; neither attempt is qualification evidence.
- **Repair and result**: `.codex-tmp/spec182-t016-r12/` used a fresh manifest with the requester role
  and current executable digest. PO-001 then completed with runner evaluation `PASS`, rc `0`, the
  `SPEC182_NATIVE_DI_REQUEST_RESULT_OK` marker, and complete namespace/process/trace/cleanup evidence.
- **Raw evidence**: preserve `r10`, `r11`, and `r12` under `.codex-tmp/spec182-t016-*`; the durable
  bounded result is [R10-B40 evidence](../specs/182-native-di-python-bindings/evidence/r10-b40-t016-po001-native-owner-pass-20260909.md).
- **Scope correction**: r12 proves an isolated native process inside an owner-created namespace;
  the PO-001 Provider callback is still supplied by the in-process integration fixture. It does not
  prove independent requester/Provider process transport.
- **Next step**: generate each remaining I01–I08/PO-002–PO-014 manifest from the current build
  identity before execution; do not reuse the stale raw manifest.

## 2026-09-09 — Spec182 R10-B41 stream business-oracle and artifact identity retry

- **Area**: T016 PO-001 stream-only owner/runner after the native result-marker repair.
- **First boundaries**: r13/r14 preserved the earlier stale-selector and missing-marker observations.
  After adding the marker to the unary and stream-only result branches, r15 still returned
  `UNQUALIFIED/MISSING_EVIDENCE:business-oracle` because its fresh runner staged the older
  `build-nac182/integration-tests` artifact. Structural observation was complete and the native
  process returned `0`; the failure was an artifact-source identity boundary, not a protocol
  failure.
- **Changed gate and result**: r16 bound the runner manifest to the actual fresh linked output
  `.codex-tmp/spec182-r4-b2/build/integration-tests` and recomputed its digest. PO-001 then
  returned `PASS` with `SPEC182_NATIVE_DI_REQUEST_RESULT_OK`, complete namespace/identity/
  process-tree/endpoint/trace/cleanup evidence, and owner exit `0`.
- **Interpretation**: close only the stream/unary business-oracle and one isolated native-process
  acceptance boundary. The Provider callback remains in the in-process fixture; independent
  requester/Provider transport, I01–I08, PO-002–PO-014, maintained caller/no-Python and full
  T016 qualification remain open.
- **Durable evidence**: [R10-B41 evidence](../specs/182-native-di-python-bindings/evidence/r10-b41-stream-business-oracle-20260909.md).
- **Raw runs**: `.codex-tmp/spec182-r10-b41/`, `.codex-tmp/spec182-t016-r13/`,
  `.codex-tmp/spec182-t016-r14/`, `.codex-tmp/spec182-t016-r15/`, and
  `.codex-tmp/spec182-t016-r16/`.

## 2026-09-09 — Spec182 R10-B50 Provider executable link closure

- **Area**: P2 preparation for an independent native Provider executable.
- **First boundary**: `./waf build --targets=di-native-provider -j2` compiled all 72 tasks but
  failed at the final link with unresolved conversation journal/wire, selection JSON, request
  envelope, and placement symbols. No Provider process or protocol request started.
- **Interpretation**: this is an examples `source` registration/link-closure miss. It is not a
  protocol result, MiniNDN result, or qualification failure. The missing translation units were
  identified by symbol-definition lookup and added to `di_native_session_sources`.
- **Raw evidence**: `.codex-tmp/spec182-r10-b50-provider-build/` plus the durable
  [R10-B50 evidence](../specs/182-native-di-python-bindings/evidence/r10-b50-provider-link-closure-20260909.md).
- **Next step**: rerun the same system-first `-j2` target build, then record a fresh source/link
  identity and run only a bounded Provider executable smoke before attempting cross-process T016.

- **Follow-up boundary**: the first repair removed the initial six unresolved symbol groups, but
  the next `-j2` link still stopped on `NativeSignedGrantRequest::sign`,
  `NativeArtifactGrantIssuer::issue`, and `planNativeRequest`. These map to
  `NativeArtifactPolicyAuthority.cpp` and `NativeRequestPlanner.cpp`; preserve this second link
  boundary in the same R10-B50 evidence before retrying.

- **Follow-up boundary 2**: after adding those two translation units, the next link stopped on
  `NativeOfferAdmission::verify`, `NativeCanonicalPreparationCatalog::bindStateContracts`,
  `NativePlanProjectionBuilder::build`, `NativeGroupKeyAdmission`, and
  `NativeGroupProjectionBuilder::build`. The definitions are in five additional native source
  files recorded in the R10-B50 Changed gate; no Provider process started.

## 2026-09-09 — Spec182 R10-B53 plan/ONNX smoke runtime loader boundary

- **Area**: local `di-native-plan-onnx-smoke` executable after its source/link build.
- **First boundary**: the target compiled and linked 86/86 tasks, but process startup failed with
  `symbol lookup error: undefined symbol: ServiceUser::publishSignedAppData`, exit `127`, before
  plan parsing or model execution. `ldd` resolved `libndn-service-framework.so.0.1.0` from
  `/usr/local/lib`; that library does not export the required symbol. No protocol or model result
  was observed.
- **Interpretation**: this is a runtime shared-library identity/RUNPATH mismatch between the
  Spec182 build tree and `/usr/local`, not an ONNX or Provider behavior failure. The raw command,
  loader output and build log are retained under `.codex-tmp/spec182-r10-b53-plan-onnx-smoke-20260909/`.
- **Changed gate before retry**: verify the candidate build-tree `libndn-service-framework.so`
  exports the symbol and run the same binary with an explicit candidate `LD_LIBRARY_PATH`; compare
  `ldd` paths and symbol lookup before classifying any smoke result. Do not overwrite the first
  `rc=127` log or treat a corrected loader path as a protocol qualification.

- **Follow-up boundary**: with the candidate library explicitly loaded, the process passed dynamic
  symbol resolution but failed during ONNX model load because manifest artifact paths are relative
  (`artifacts/qwen-native-tracer-backbone.onnx`) and the retry was launched from the repository
  root. Exit `2`; no role executed. The output is retained as `smoke-candidate-lib.log` in the same
  raw run directory. The changed gate is to launch from the bundle root and recheck `ldd` selects
  the candidate framework library before interpreting model/session behavior.

- **RPATH retry command boundary**: after adding the target `$ORIGIN/..` RUNPATH, the relink and
  `ldd` check succeeded, but the first no-`LD_LIBRARY_PATH` retry expanded `$PWD` after changing
  into the bundle directory and therefore pointed at a nonexistent nested binary path (`rc=127`).
  This is a harness command construction error; its raw output is in
  `.codex-tmp/spec182-r10-b53-rpath-retry-20260909/`. The corrected retry fixes the repository
  root path before `cd` and keeps the bundle cwd only for artifact resolution.

## 2026-09-09 — Spec182 R10-B54 plan/manifest smoke source-closure boundary

- **Area**: local `di-native-plan-manifest-smoke` executable covering native plan/manifest
  parsing, role registration and dependency publication.
- **First boundary**: the initial 55/55-task `-j2` link stopped with unresolved native ONNX
  planning/recipe helpers and `ServiceUser` publication/collaboration methods. No smoke process
  or protocol request started; the raw linker output is retained in
  `.codex-tmp/spec182-r10-b54-plan-manifest-smoke-20260909/build.log`.
- **Interpretation**: this was a target source/link closure miss. The target's source list omitted
  `di_native_onnx_assembly_sources`, and its link closure omitted the candidate
  `ndn-service-framework`/ONNX/Protobuf dependencies. It is not a plan, manifest or protocol
  behavior result.
- **Changed gate before retry**: map every unresolved project symbol to its defining translation
  unit, add that source set and the shared framework/dependency closure to `examples/wscript`,
  add the target RUNPATH, then run a fresh target link and default-loader `ldd` check before
  interpreting smoke output. This source-definition map and target registration check is now a
  required feedback item for the shared Spec Kit build/source-closure gate.

## 2026-09-09 — Spec182 R10-B55 Provider serve preflight argument boundary

- **Area**: bounded startup probe for the repaired standalone Provider `--serve` path.
- **First boundary**: the first probe supplied plan/manifest and serving options but omitted the
  required `--serve` mode flag. `parseArgs` rejected the invocation with
  `exactly one of --check-only or --serve is required` (exit `2`), before Face creation or any
  Provider registration. Raw output is retained in
  `.codex-tmp/spec182-r10-b55-provider-serve-preflight-20260909/serve.log`.
- **Interpretation**: this is a harness command-construction/CLI boundary, not a serving,
  permission, protocol or qualification result.
- **Changed gate before retry**: assert the explicit `--serve` mode in the command and retain the
  metadata-only manifest (no preassembled artifact paths); classify only startup markers and the
  first NFD/certificate/permission/readiness boundary from the corrected bounded probe.

- **Follow-up collection boundary**: the Controller-assisted retry itself reached
  `NDNSF_DI_NATIVE_PROVIDER_READY` and the Controller exited cleanly, but the command's post-run
  `rg` marker extraction returned `command not found` because the runtime-only PATH intentionally
  omitted the developer `rg` location. Provider/Controller logs and exit codes were preserved;
  marker extraction was repeated with system `grep`/direct file reads. This is a harness collection
  issue, not a Provider or permission result.

## 2026-09-09 — Spec182 R10-B75 Provider failure-exit repair build boundary

- **Area**: Provider failure-observability repair after the R10-B74 static audit.
- **First boundary**: the first rebuild after adding the scheduler-stop API failed during
  `ServiceProvider.cpp` compilation because `ndn::scheduler::ScopedEventId` has no `reset()`
  member. No executable was linked from that attempt. The raw command and compiler boundary are
  retained under `.codex-tmp/spec182-r10-b75-provider-failure-exit-20260909-r0-build-j4-fail/`.
- **Changed gate before retry**: check the exact ndn-cxx handle API, replace `reset()` with move
  assignment to an empty scoped event, and reduce the next build to `-j2` after `vmstat` showed
  sustained swap-in/out on this host.
- **Follow-up behavior boundary**: after the compile repair, the first standalone Provider
  failure probe still timed out because the Face event loop was restarted by the NDNSF heartbeat
  and permission retry schedulers. The raw timeout runs are retained under
  `.codex-tmp/spec182-r10-b75-provider-failure-exit-20260909-r1` through `r10`.
- **Final result**: stopping the Face `io_context` from the failure task, then cancelling the
  Provider heartbeat scheduler on the main thread, produced rc=2 in 1.08 seconds after
  `NDNSF_DI_NATIVE_PROVIDER_PROVISION_FAILED`. This closes failure observability for this bounded
  CLI boundary; it does not establish requester/Provider transport or qualification.

## 2026-09-10 — Spec182 R10-B80 finite Provider probe startup boundary

- **Area**: finite standalone Provider serve probe after the R10-B78 run-limit cancellation repair.
- **First boundary**: the first probe used the fresh Provider binary without a running local
  Controller. Face and serving markers were emitted, then the event loop raised
  `Failed to fetch public parameters after multiple attempts.` and the process returned rc=2
  before the run-limit marker. Raw output is retained in
  `.codex-tmp/spec182-r10-b80-provider-run-limit.log` with its rc record.
- **Interpretation**: this is a local Controller/public-parameter startup boundary, not evidence
  against the run-limit repair or a protocol result.
- **Changed gate before retry**: start the real local `App_ServiceController` with the recorded
  temporary policy, keep the four-role plan and metadata-only manifest unchanged, and classify
  only the Provider `SERVE_READY`, run-limit, permission-wait cancellation, and process exit
  markers.
- **Follow-up result**: the Controller-assisted probe reached `SERVE_READY`, emitted
  `RUN_LIMIT_REACHED` and `PERMISSION_WAIT_CANCELLED`, and Provider/Controller both exited
  rc=0. Raw output is retained under
  `.codex-tmp/spec182-r10-b80-provider-run-limit-controller/`; this remains a bounded lifetime
  result and not requester/Provider transport qualification.

## 2026-09-10 — Spec182 R11-B8-G6 native process fixture socket-path boundary

- **Area**: fresh C++ requester/Provider conversation process validation.
- **First boundary**: NFD exited before creating its Unix socket with
  `File name too long`; the retained run root made the generated socket path
  exceed the Unix-domain pathname limit. The driver was stopped while waiting
  for the socket (exit 130).
- **Interpretation**: fixture startup failure only. No Controller, Authority,
  grant, selection, stream, or conversation result is counted.
- **Corrective gate**: rerun unchanged binaries under a short `/tmp` run root,
  preserve the raw run and link this evidence before evaluating protocol
  behavior.
- **Evidence**:
  [`r11-b8-g6-native-process-fixture-startup-20260910.md`](../specs/182-native-di-python-bindings/evidence/r11-b8-g6-native-process-fixture-startup-20260910.md)

- **Corrective result**: the shared process fixture now selects a short
  per-process `/tmp` socket pathname whenever the retained run root is too
  deep. The corrected conversation/recovery and unary process runs reached
  their declared C++ protocol boundaries; see G7/G8 evidence.

## 2026-09-10 — Spec182 R11-B1 independent authority process fixture boundaries

- **Area**: C++ requester to independent `DI_NativeArtifactAuthority` grant process over a
  private NFD and Controller.
- **First boundaries**: the initial fixture shared one writable PIB/TPM among Controller,
  Authority and requester. Authority startup raced ndn-cxx identity/default-certificate state;
  Controller then reported a missing signing certificate and Authority could not fetch PUBPARAMS.
  After switching the requester namespace to read-only, its HOME was also read-only and the C++
  probe stopped at `Failed to acquire file lock`. A copied identity store then retained the
  bootstrap `tpm-file:` locator, so Authority reached permission fetch but could not decrypt the
  Controller response. Raw runs are retained under `/tmp/spec182-r11-b1-process-rpziurt1`,
  `/tmp/spec182-r11-b1-process-wqjaczju` and `/tmp/spec182-r11-b1-process-1c19cxaj`.
- **Interpretation**: these were process-fixture identity-storage and namespace boundaries;
  none was a grant protocol result. The Controller-only run passed before Authority was added.
- **Changed gate before retry**: generate NDN identities once, copy the full PIB/TPM per native
  role, rewrite each copied PIB's TPM locator, retain only that role's private NDN key files,
  keep requester HOME on tmpfs, and leave Authority grant private material outside requester
  mounts. Only the corrected fixture's C++ positive/negative markers count as R11-B1 evidence.
- **Final result**: the corrected independent process fixture passed one positive grant, five
  Authority-handler rejection cases (bad signature, wrong epoch, unknown recipient, malformed
  wire, expired grant) and one Authority-unreachable transport timeout. R11-B2 onward and T016
  remain open.

- **Related batch-end check**: one concurrent `Spec182*` unit-suite run returned rc=201 with a
  memory access violation in the pre-existing `Spec182V3Placement/PublicClientConversationCommitsSeededReceiptAndCheckpoint`
  fixture at `tests/unit-tests/di-native-v3-placement.t.cpp:1006`. The selector passed ten isolated
  reruns, and a fresh complete `Spec182*` rerun passed 256/256 cases and 7077/7077 assertions;
  raw output is retained at `.codex-tmp/spec182-r11-b1-spec182-unit-rerun.log`. This is retained as
  an intermittent full-suite fixture boundary, not counted as a process protocol failure or as a
  reason to alter the R11-B1 production path.

## 2026-09-10 — Spec182 R11-B8-G11 reservation-overflow guard compile boundary

- **Area**: native Provider co-location capacity accounting.
- **First boundary**: the first rebuild after adding checked cumulative reservation arithmetic
  failed in `NativeV3Placement.cpp`; a multi-statement block was introduced under an existing
  single-line `if` without braces, producing a misleading-indentation warning and a map key type
  compile error. No binary from this attempt was used for behavior claims.
- **Interpretation**: implementation compile failure only; no request, Provider, NDN, or
  qualification result was produced.
- **Correction before retry**: add explicit braces around the guarded reservation block, retain
  the failed build directory, then rebuild the same Waf target with the system-first `-j2` command.
- **Evidence**:
  [`r11-b8-g11-provider-colocation-20260910.md`](../specs/182-native-di-python-bindings/evidence/r11-b8-g11-provider-colocation-20260910.md)

## 2026-09-10 — Spec182 R11-B9-G3 independent process dependency boundary

- **Area**: fresh C++ requester/Provider cross-process conversation validation.
- **First boundary**: the Python driver failed while importing the current `_ndnsf`
  extension because the environment selected `/usr/local/lib/libnac-abe.so`, which lacks
  `ndn::nacabe::Consumer::clearCache(...)` required by the build. The run returned `rc=1`
  before Controller, Authority, Provider, requester, or NFD protocol startup.
- **Interpretation**: dynamic-loader dependency mismatch only; no protocol result is counted.
  Raw command, output, return code, and `ldd` capture are retained under
  `.codex-tmp/spec182-r11-b9-cross-process-current-20260910-r0/`.
- **Changed gate before retry**: put the matching
  `/home/tianxing/NDN/nac-abe-integration-182/install/lib` and
  `/home/tianxing/NDN/ndn-svs/build` prefixes first in `LD_LIBRARY_PATH`; retain
  `/usr/local/lib` only after them for remaining ndn-cxx/ndnsd dependencies.
- **Evidence**:
[`r11-b9-g3-cross-process-dependency-boundary-20260910.md`](../specs/182-native-di-python-bindings/evidence/r11-b9-g3-cross-process-dependency-boundary-20260910.md)

## 2026-09-11 — Spec184 T007 candidate rebuild disk-space boundary

- **Area**: fresh candidate source/build closure after the NativeProvider input
  binding, preassembled runtime dispatch and parser-fuzz selector changes.
- **First boundary**: `WAFLOCK=.lock-spec184-b5 ./waf -o
  build-spec184-b5-candidate build -j4` stopped at 117/842 tasks because the
  compiler could not write temporary assembly files and the linker reported
  `No space left on device`; no candidate binary from this attempt was used for
  behavior or qualification claims.
- **Interpretation**: host storage exhaustion only; no C++ test, protocol,
  process or qualification result was produced by the failed build.
- **Correction before retry**: preserve the raw failed command boundary, remove
  only reproducible untracked `build-*` directories (not `.codex-tmp` evidence,
  source or user files), verify 18 GiB free space, then rerun the same
  system-first `-j4` candidate build. The candidate digest must be regenerated
  after the successful rebuild.

## 2026-09-11 — Spec184 T007 candidate auxiliary-target link boundary

- **Area**: fresh candidate build after the parser-fuzz and native runtime fixes.
- **First boundary**: after a fresh configure, the broad `build -j4` reached
  the historical `spec181-assembly-parity` link target and failed with missing
  `NativeModelDescriptor::validate`, `NativeAdapterDescriptor::descriptorDigest`
  and related planning symbols. No Spec184 unit/integration/provider/requester
  executable from this attempt was used for qualification.
- **Interpretation**: an unrelated historical auxiliary-target link closure;
  no protocol or C++ behavior result was produced.
- **Correction before retry**: retain the configured tree and build only the
  Spec184 candidate targets (`unit-tests`, `integration-tests`,
  `DI_NativeRequester`, and `di-native-provider`) with the same system-first
  `-j4` toolchain. The skipped auxiliary target remains an explicit build
  limitation rather than a qualification PASS.

## 2026-09-11 — Spec184 T007 unit runner auxiliary-path boundary

- **Area**: full C++ unit qualification against the fresh candidate tree.
- **First boundary**: the candidate built `DI_NativeOnnxAssemblyWorker` and all
  `spec182-worker-tool-*` helpers, but the unit test's default lookup only
  searched `build-nac182/`, `build/` and the invocation directory. Two runs
  therefore stopped with 15 `spec182 worker binary not found` fatal test
  messages and exit `201` before those subprocess cases executed.
- **Interpretation**: test-to-candidate path binding only; no worker protocol
  assertion or native production result was produced by those cases.
- **Correction before retry**: set `NDNSF_SPEC182_BIN_DIR=build-spec184-b5-candidate`
  explicitly, retain both raw runs, and rerun the same unit executable. The
  path override is part of candidate identity for the qualification record.

## 2026-09-11 — Spec184 T007 owner output preflight boundary

- **Area**: fresh candidate-bound MiniNDN owner/runner case.
- **First boundary**: the owner was invoked with an output directory that had
  already been created by the caller. Its explicit new-output guard returned
  exit `2` before topology creation, process staging, namespace observation or
  business execution.
- **Interpretation**: harness invocation mistake only; no owner or native
  protocol result was produced.
- **Correction before retry**: retain the empty failed directory and invoke the
  owner with a fresh path that does not exist. The runner manifest is the
  recomputed current-candidate manifest with all 34 artifact digests refreshed.

## 2026-09-11 — Spec184 T007 owner privilege boundary

- **Area**: candidate-bound MiniNDN namespace owner rerun.
- **First boundary**: the fresh-path invocation reached the owner's explicit
  privilege check and returned `MININDN_REQUIRES_ROOT` / exit `2`; this session
  runs as uid 1000 and cannot create the required root network namespaces.
- **Interpretation**: external owner privilege boundary; no MiniNDN topology,
  native process, or protocol result was observed for the new candidate.
- **Disposition**: retain the raw result at
  `.codex-tmp/spec184-b5-owner-probe-20260911-r4/result.json` and keep the
  process/no-Python row `PARTIAL`. The earlier root owner result remains bound
  to its historical candidate and is not silently reused.

## 2026-09-11 — Spec184 T007 owner runtime-PATH boundary and corrected root owner result

- **Area**: current-candidate MiniNDN owner/runner `PO-001-stream`.
- **First boundary**: a root retry with a new output directory still returned
  `MININDN_OWNER_FAILED:JSONDecodeError`; tracing the owner call showed that
  `infoconv` was missing from the command-local `PATH`. No runner process or
  business result was produced by that retry. The raw output is retained at
  `.codex-tmp/spec184-b5-owner-probe-20260911-r5-owner.log` and the command at
  `.codex-tmp/spec184-b5-owner-probe-20260911-r5-command.txt`.
- **Correction and result**: rerun as root with `/usr/local/bin` restored in
  `PATH`, using the same current runner manifest and a new output directory.
  The canonical two-node owner created complete node context, trace, process,
  endpoint and cleanup evidence; the runner and business marker passed with
  exit `0`. Evidence is under
  `.codex-tmp/spec184-b5-owner-probe-20260911-r7/`; the result digest is
  `e65fc1b507fe40cc275601b031c724b99f64a254ecaaa51272a7845f8e5509fd` and
  the runner-result digest is
  `7cc45f6f9d4487ffe45de90dff37c6bc2e28e030295d69e6b31aa2c48186a0b9`.
- **Interpretation**: the corrected root owner result is valid for the fresh
  local candidate's bounded `PO-001-stream` row only. It does not close I02–I08,
  real-model breadth, Python retirement or external SIF/Tiger qualification.

## 2026-09-11 — Spec184 T007 process refresh invocation boundaries

- **Python environment boundary**: the first root unary process attempt stopped
  before Controller/Authority/Provider startup because the command-local
  `PYTHONPATH` did not expose the installed `ndn` module (`ModuleNotFoundError`).
  The raw run remains at `.codex-tmp/spec184-process-unary-20260911-r1/` and is
  not a protocol result. The corrected retry added the pinned MiniNDN Python
  paths and produced the C++ unary oracle result recorded in the T007 process
  evidence.
- **Waf target-name boundary**: the first final candidate build requested the
  non-generator target `DI_NativeOnnxAssemblyWorker`; Waf rejected the target
  name before compilation. Raw output is
  `.codex-tmp/spec184-final-target-build-20260911-r1.log` (SHA-256
  `06eaa5a90fc70fe5cda2e5c3c8d7cb809c3d859432ce62dedcad2ee8e91f9a02`). No
  candidate behavior result came from this attempt. The corrected target set
  completed with `-j4` in `r2`.
- **Owner manifest boundary**: the first fresh-candidate root owner retry used
  a stale integration executable digest and stopped at `artifact digest
  mismatch` before MiniNDN process execution. The raw result is retained at
  `.codex-tmp/spec184-owner-po001-20260911-r8/`. The runner manifest was
  regenerated from the current candidate outputs before the valid `r9` owner
  run; the stale attempt is not combined with the valid result.

## 2026-09-11 — Spec184 T007 I02-I08 dynamic sample invocation boundaries

- **I02 runtime-PATH boundary**: the first I02 owner invocation stopped before
  MiniNDN topology creation because the command-local `PATH` omitted `/sbin`,
  so `ifconfig` could not be found. The raw run is retained at
  `.codex-tmp/spec184-owner-i02-20260911-r1/` and is not a protocol result.
- **I02 fixture-staging boundary**: after correcting `PATH`, the C++ selector
  exited `201` because the tiny role ONNX fixtures were not staged. The raw
  owner result is `.codex-tmp/spec184-owner-i02-20260911-r2/`; no business
  result is inferred from it.
- **I02 manifest-schema boundary**: after staging the fixtures, the runner
  rejected the registration manifest's unsupported `fixture` artifact kind
  before execution. The raw result is
  `.codex-tmp/spec184-owner-i02-20260911-r3/`. The manifest was corrected to
  use the declared `data` kind before the valid `r4` run.
- **Disposition**: these three boundaries are harness/setup failures. The
  corrected I02 `r4` and I03–I08 `r1` owner runs are recorded as
  `PASS_FOR_DYNAMIC_SAMPLE`; they do not close the inherited isolation
  counterexample or collector-completeness rows.

- **Follow-up counterexample probe**: an exploratory renamed-Python-ELF case
  first stopped because the runner manifest used the default `bwrap` name while
  the staged runtime required `/usr/bin/bwrap`; raw output is
  `.codex-tmp/spec184-owner-i02-python-elf-20260911-r1/`. After the tool paths
  were made explicit, the ELF reached the isolated runtime but exited before
  its stdout oracle because the staged root had no Python standard-library
  files; the collector observed `PYTHON_MAPPING` and classified the result
  `UNQUALIFIED`. Raw reruns are retained at
  `.codex-tmp/spec184-owner-i02-python-elf-20260911-r2/` and `r3/`. These
  probes confirm a runner/fixture closure gap and are not I02 protocol results.

## 2026-09-11 — Spec184 candidate-bound C++ isolation counterexample boundaries

- **I02 fork/helper**: the C++ fixture forked and successfully executed a second in-root helper;
  the strengthened collector classified the complete observation as `FAIL / UNDECLARED_EXEC`,
  even though the fixture emitted its business marker.
- **I03 Python mapping**: the C++ fixture opened the staged `libpython3.8.so.1.0` by a runtime-built
  path; the collector classified the complete observation as `FAIL / PYTHON_MAPPING`.
- **I04 endpoint**: the fixture made a failed loopback TCP `connect` to an undeclared endpoint;
  failed attempts are now retained and classified as `FAIL / UNDECLARED_ENDPOINT`.
- **I05 observer budget**: the trace exceeded the declared 4096-byte budget; the result is
  `UNQUALIFIED / TRACE_BUDGET_EXCEEDED`, not a protocol `FAIL`.
- **I06 cold/role**: the C++ cold case omitted the required Provider process; role coverage produced
  `FAIL / ROLE_COVERAGE_MISMATCH`.
- **I07 external harness**: Python remained only the owner-side harness and the C++ business marker
  passed; the result is `PASS` for this bounded counterexample.
- **I08 detached child**: the fixture detached a sleeping descendant; terminal/descendant evidence
  produced `FAIL / OWNED_PROCESS_ALIVE`. The owner cleanup left no counterexample process behind.

The corrected owner outputs are retained under
`.codex-tmp/spec184-counterexamples-live-20260911/owner-i02-r3/`,
`owner-i03-r4/`, `owner-i04-r3/`, `owner-i05-r3/`, `owner-i06-r3/`,
`owner-i07-r3/`, and `owner-i08-r4/`. Earlier manifest/tool-path attempts remain separate raw
setup failures. These runs close only the bounded counterexample classes; real-model breadth,
Python retirement and external SIF/Tiger ownership remain open in Spec184 T007.

## 2026-09-11 — Spec184 targeted C++ qualification transient failure

- **Targeted-suite boundary**: a first combined run of 151 current-candidate
  C++ unit cases stopped in
  `Spec182V3Placement/PublicClientConversationCommitsSeededReceiptAndCheckpoint`
  after `DI_NATIVE_OFFER_REJECTED`, followed by a memory-access violation.
  The raw run is retained at
  `.codex-tmp/spec184-b5-targeted-unit-20260911/unit.log`.
- **Isolation result**: the named V3 case passed in three fresh single-case
  runs, and the same 151-case selector passed in three fresh reruns. This
  leaves a transient/order-sensitive boundary requiring a sanitizer or
  repeated stress run before it can be treated as closed; the failed run is
not a qualification PASS and does not change T007 status.

## 2026-09-11 — Spec184 full-unit runner environment boundary

- **Missing worker-directory binding**: a full current-candidate unit sweep was
  first invoked without `NDNSF_SPEC182_BIN_DIR`. Fifteen ONNX worker/activation
  cases stopped before their subject logic because the required worker tools
  were not found, producing exit `201`. Raw output is retained at
  `.codex-tmp/spec184-b5-full-unit-rerun-20260911/unit.log`.
- **Disposition**: this is a test-entry configuration failure, not a native
  protocol result. The corrected invocation with
  `NDNSF_SPEC182_BIN_DIR=build-spec184-b5-candidate` completed the full unit
  sweep with exit `0`; the corrected result is bound in Spec184 T007 evidence.

## 2026-09-11 — Spec184 T007 YOLO registration-handle lifetime boundary

- **Initial Y-A boundary**: the Controller and second ServiceUser process
  aborted with `corrupted size vs. prev_size` before the case reached its
  business oracle.  The raw runs are retained at
  `.codex-tmp/spec184-yolo-Y-A-run-20260911-r2.log` and
  `.codex-tmp/spec184-yolo-Y-A-run-20260911-r8.log`; the gdb traces are
  `.codex-tmp/spec184-yolo-Y-A-gdb-attach-20260911-r4.log` and
  `...-r9.log`.
- **Cause and correction**: `Face::setInterestFilter` returns a
  `RegisteredPrefixHandle` whose scoped wrapper unregisters on destruction.
  Several production registrations discarded that handle while the async NFD
  command was still pending.  The retry helper, Controller, User, Provider and
  certificate publisher now retain scoped handles for their registration
  lifetime.  The focused registration selector passed after this correction.
- **Follow-up boundary**: with the handle correction, Y-A r11 passed the
  previous Controller/User startup boundary but the Repo child exited with
  return code `-6` after `register prefix failed` and `corrupted size vs.
  prev_size`, before readiness.  Raw output is retained at
  `.codex-tmp/spec184-yolo-Y-A-run-20260911-r11.log` and its output tree
  `.codex-tmp/spec184-yolo-Y-A-output-20260911-r11/`.  This remains a
  process-start/preflight `UNQUALIFIED` result, not a protocol result; the
  next retry must first preserve and inspect this new boundary.
- **Model boundary**: this host can execute only the cached `Qwen3-0.6B`
  smoke/ABI fixture.  It cannot execute the contract-required
  `Qwen/Qwen3.6-27B`; no local 0.6B result may be relabeled as that
  qualification row.

## 2026-09-11 — Spec184 registration-handle focused build invocation boundary

- The first affected-target retry used `./waf -o build-spec184-b5-candidate
  build` without the configured-output option expected by this Waf tree; it
  stopped before compilation with `The project was not configured`.  Raw
  output: `.codex-tmp/spec184-registration-handle-build-20260911-r1.log`.
  This is a build-command/setup boundary, not a source or protocol result;
  the candidate uses the non-default Waf lock `.lock-spec184-b5`, so the next
  invocation must set `WAFLOCK=.lock-spec184-b5` (or explicitly configure that
  output) before building.  The second probe without that lock is retained at
  `.codex-tmp/spec184-registration-handle-build-20260911-r2.log`.

- The correctly locked `-j2` retry entered the changed User/Provider sources
  but its launcher disappeared while compiling `ServiceController.cpp`, with
  no Waf completion or compiler diagnostic in
  `.codex-tmp/spec184-native-receipt-build-20260911-r7.log`.  The partial
  object is not treated as a build result or candidate identity.  Because the
  host was under several GiB of swap pressure, the next retry is serialized at
  `-j1` and its process outcome will be recorded separately.

- The serialized `-j1` retry again stopped after the Waf progress line for
  `ServiceProvider.cpp`, before Waf completion, with no compiler diagnostic or
  exit record in `.codex-tmp/spec184-native-receipt-build-20260911-r8.log`.
  No candidate receipt was generated.  This is retained as an incomplete
  launcher/session boundary; a detached build with an explicit PID and log is
  required to distinguish host-session loss from a compiler failure.

## 2026-09-11 — Spec184 T007 Y-A state-root ownership boundary

- The first post-receipt Y-A invocation stopped before MiniNDN startup with
  `STATE_ROOT_OWNER_MISMATCH`: the fresh state directory had been created by
  uid 1000 while the authorized MiniNDN owner is root.  Raw output is
  `.codex-tmp/spec184-yolo-Y-A-run-20260911-r12.log`.  This is a runner
  preflight boundary, not a protocol or model result; the retry must create
  both state and output roots under the actual root owner and retain mode
  `0700`.

## 2026-09-11 — Spec184 T007 Y-A Controller startup heap-corruption boundary

- After correcting the state/output roots to root-owned `0700`, the next
  candidate-bound Y-A run stopped during the `control` phase with
  `CASE_RUNTIME_PROCESS_START_FAILED:control`.  The runner diagnostic is
  retained at `.codex-tmp/spec184-yolo-Y-A-run-20260911-r13.log` and
  `.codex-tmp/spec184-yolo-Y-A-output-20260911-r12/process-start-failure.json`.
- The Controller log shows the first two `CertificatePublisher` prefixes
  reported registered, then `corrupted size vs. prev_size`, before Controller
  readiness; no Repository/Provider/User process or business oracle was
  observed.  The raw child log is
  `.codex-tmp/spec184-yolo-Y-A-output-20260911-r12/controller.log`.
- This is a native process-start/preflight `UNQUALIFIED` boundary, not a
  protocol result.  The registration-handle change therefore remains under
  diagnosis and must pass a focused reproducer or sanitizer review before
  another YOLO retry.  This host still supports only Qwen3-0.6B smoke/ABI
  checks; Qwen/Qwen3.6-27B remains an external-owner row.

## 2026-09-11 — Spec184 T007 dependency/build retry boundaries

- A temporary gdb wrapper initially failed during module loading because the
  copied module was not inserted into `sys.modules`; raw output is
  `.codex-tmp/spec184-yolo-Y-A-run-20260911-r14.log`.  No MiniNDN or product
  process ran in that diagnostic attempt.
- The corrected gdb run reproduced the Controller abort and captured the
  `malloc_printerr` stack through `ndn::Face::Impl::registerPrefix`; raw output
  is `.codex-tmp/spec184-yolo-Y-A-run-20260911-r15b.log`, with child log under
  `.codex-tmp/spec184-yolo-Y-A-output-20260911-r15/controller.log`.  This
  confirmed an unretained NAC-ABE DKEY filter, which was repaired and rebuilt.
- The first fresh NDNSF build against `install-spec184-r4` completed Waf but
  stopped because the build helper omitted `ndnsf-distributed-inference` from
  its target list, so setup.py could not find the DI shared library.  Raw log:
  `.codex-tmp/spec184-native-r4-build-r2.log`.  The helper now requests that
  target explicitly; the corrected candidate build and verify completed.
- The first Y-A run against the corrected candidate reached Controller and
  Repository startup without heap corruption, then remained on the existing
  default generation-state lock (`Controller generation writer unavailable`)
  and was terminated after its bounded permission retries.  Raw output is
  `.codex-tmp/spec184-yolo-Y-A-run-20260911-r16.log`; this is a startup/state
  precondition boundary, not a protocol or model result.  The next retry uses
  a fresh case-scoped `NDNSF_CONTROLLER_GENERATION_STATE` path.

## 2026-09-11 — Spec184 T007 Y-A native assembly-worker boundary

- With a fresh generation-state file, Y-A completed Controller, Repository,
  Provider startup, catalogue publication, permission, ACK and Selection.  The
  Provider then reported `DI_PROVIDER_ASSEMBLY_WORKER_LOCATION_MISSING`; the
  User exited `1` after the selection-status timeout.  Raw output is
  `.codex-tmp/spec184-yolo-Y-A-run-20260911-r17.log`, with child evidence under
  `.codex-tmp/spec184-yolo-Y-A-output-20260911-r17/`.
- The Provider log identifies the immediate cause: the fresh candidate did not
  contain `DI_NativeOnnxAssemblyWorker`.  Its Waf target was present but was
  omitted from the native build helper's target list, so the executable could
  be absent while the shared libraries and Provider were present.  This is a
  candidate artifact/assembly precondition, not a model or protocol result.
- The native build helper now requests the DI shared library, native requester,
  artifact authority, assembly worker and Provider together.  The Y-A row stays
  `UNQUALIFIED` until a fresh candidate containing that worker reaches the
  terminal numerical oracle.

## 2026-09-11 — Spec184 T007 Y-A retry state-root ownership boundary (r18)

- The worker-pinned retry stopped before MiniNDN startup with
  `STATE_ROOT_OWNER_MISMATCH`: the newly created state/output roots were made
  by the invoking user while the `sudo` MiniNDN runtime requires root-owned
  `0700` directories.  Raw runner output is
  `.codex-tmp/spec184-yolo-Y-A-run-20260911-r18.log`.
- No Controller, Repository, Provider, User, protocol, or model activity was
  observed.  This remains a runner preflight boundary; the next retry must
  create the case roots under the actual runtime owner before evaluating the
  worker binding.

## 2026-09-11 — Spec184 T007 YOLO matrix collector boundary and closure

- Current-candidate Y-B completed its native four-Provider terminal path and
  numerical oracle in `.codex-tmp/spec184-yolo-Y-B-output-20260911-r37/`.
  Current-candidate Y-A subsequently completed the single-Provider path in
  `.codex-tmp/spec184-yolo-Y-A-output-20260911-r51/`; both are recorded as
  `PASS_FOR_ROW` in the active Spec evidence.
- The first complete Y-N attempt (`r48`) reached all three E mutation Provider
  decisions, but the collector stopped at `Y_N_MATRIX_INCOMPLETE:Y-N-E` because
  Provider RuntimeEvidence lines carry an ndn-cxx timestamp/logger prefix and
  the collector required `NDNSF_DI_GRANT_VERIFICATION` at byte zero.  Raw
  output remains `.codex-tmp/spec184-yolo-Y-N-run-20260911-r48.log` and its
  variant directories; this is a harness observation boundary, not a product
  rejection failure.
- The collector now normalizes the registered marker suffix while preserving
  the JSON binding checks.  A fresh Y-N run (`r50`) produced
  `SPEC180_CASE_RESULT status=PASS case=Y-N`; all seven subcases and
  `EXPIRED`/`FORGED_AUTHORITY`/`WRONG_RECIPIENT` Provider mutations pass, with
  no cleanup errors.  See
  `.codex-tmp/spec184-yolo-Y-N-output-20260911-r50/y-n-matrix-result.json` and
  `specs/184-native-di-closure/evidence/t007-process-qualification-20260911.md`.

## 2026-09-12 — Spec184 T007 local model-capability and Waf target boundary

- The current host can run only the user-provided `Qwen3-0.6B` smoke/ABI
  fixture; it cannot execute the contract-required `Qwen/Qwen3.6-27B`. The
  exact model manifest, tokenizer, CUDA runtime and staged objects therefore
  remain an external-owner input. The 0.6B result must not be relabeled as 27B
  qualification. See
  `specs/184-native-di-closure/evidence/t007-model-capability-20260912.md`.
- A bounded attempt to add `integration-tests` to the r4 candidate stopped
  before compilation with `Could not find a task generator for the name
  'integration-tests'` because that candidate was configured with
  `--with-examples`. Raw output is
  `.codex-tmp/spec184-qwen-smoke-20260912/waf-build.log` (SHA-256
  `1f49b7cd938da12e8169c4248501b832b85b8fcdb66b2fc1352092dd413ece62`). This
  is a Waf configuration boundary, not a model, protocol or C++ runtime result;
  no reconfiguration was performed merely to manufacture a selector binary.
- A follow-up launch mixed the older candidate's `integration-tests` executable
  with r4 libraries. It emitted `SPEC182_NATIVE_DI_REQUEST_RESULT_OK` and then
  SIGSEGVed at `0x00000080` (exit `201`). The mixed ABI is rejected as evidence;
  raw output is `.codex-tmp/spec184-qwen-smoke-20260912/integration-qwen.log`
  (SHA-256 `33c155fb5124cd7249551586e6b5ee1b5658334503d05ac09d4ec8f4d0a85cde`).

## 2026-09-12 — Spec185 B1 install/build boundaries

- The first B1 `waf install` entered the full historical build graph and
  stopped at the unrelated `spec181-assembly-parity` link with missing
  `NativeModelDescriptor`/planning symbols. The target-limited retry installed
  the native libraries, then stopped at the repository's Python editable-install
  hook. Raw logs are `.codex-tmp/spec185-b1-runtime-consumer/install.log` and
  `install-target.log`; neither boundary is counted as a B1 Runtime product
  failure.
- B1 was subsequently validated with the already successful DI/Core build and
  an isolated installed-prefix C++ consumer. The consumer result and the
  explicit linker/toolchain harness boundary are recorded in
  `specs/185-prepared-model-runtime/evidence/b1-runtime.md`.

## 2026-09-12 — Spec185 B2E normal selector fixture boundary

- The first B2E normal build linked `spec185-extension-registry` successfully,
  but its selector stopped in two Qwen fixture cases with
  `Qwen native splitter role cover is incomplete`. The fixture supplied two
  roles while relying on the constructor's three-role default tensor-degree
  vector; no production request or publication path ran. Raw output is
  `.codex-tmp/spec185-b2e/normal-runtime.log`.
- The boundary is classified as a C++ test-fixture contract error. The fix
  supplies the explicit two-element tensor-degree vector and must pass the
  read-only static re-review before the selector is retried. The failed run is
  not a product PASS and B2E remains open.

- After rebuilding with that fixture fix, the selector reached the Qwen
  `inspectGraph` identity check and stopped with `graph snapshot identity or
  topological order is invalid`: the test model used an arbitrary graph digest
  instead of the canonical digest for the requested revision and layer ranges.
  Raw output is `.codex-tmp/spec185-b2e/normal-runtime-fixture-fix.log`.
- This remains a C++ fixture contract boundary. The test now derives the
  canonical Qwen graph digest before calling `inspectGraph`; the production
  graph identity check is retained. B2E remains open pending static re-review
  and a fresh build/runtime result.

- The first B2E installed-prefix consumer attempt reached C++ link but used
  Linuxbrew `ld` through the ambient `g++` PATH. It consequently reported
  missing system protobuf, Boost filesystem, OpenSSL and ONNX Runtime symbols;
  the source-tree leak checks had not failed. Raw output is
  `.codex-tmp/spec185-b2e/installed-consumer-r2.log`.
- This is a toolchain-harness boundary, not a DI API failure. The retry uses
  the repository-required system-first PATH and keeps the installed prefix
  and library/header identity unchanged.

## 2026-09-13 — Spec185 B2 first compile boundary

- The first B2 shared `-j4` compile stopped before linking at the existing
  Core `OperationRuntime.cpp` changes: `notifyWaiters()` was still defined
  inside the anonymous namespace, the three-argument `drainAsync` definition
  had no matching declaration, and the local `Notification` aggregate was
  constructed with arguments despite having no constructor. No B2 selector
  ran and no product result is inferred. Raw output is
  `.codex-tmp/spec185-b2/normal-build.log`; the baseline memory sample is
  `.codex-tmp/spec185-b2/vmstat-before.log`.
- This is a compile/link source-boundary failure. The fix must move the member
  definition out of the anonymous namespace, align the overload set, and use
  aggregate initialization (or an explicit constructor), then pass the
  affected Core static review before retrying the B2 build.

- The first retry compiled the repaired Core source but stopped before link at
  two additional interface boundaries: restoring the three-argument overload
  alongside a four-argument overload with a default parameter made explicit
  three-argument calls ambiguous, and the DI `User` caller lacked access to
  the private Core timer scheduler. The T004 target did not link and no
  selector ran. Raw output is `.codex-tmp/spec185-b2/normal-build-v2.log`.
- This remains a compile-only API/ownership boundary. The retry removes the
  four-argument default (keeping both ABI overloads distinct) and grants the
  DI `User` class the private scheduling friend access without widening the
  Core public API; the affected Core/DI range requires static re-review before
  another build.

## 2026-09-13 — Spec185 B2 first native selector fixture boundary

- The first normal `Spec185Preparation` selector reached the native catalog
  path but stopped with `graph snapshot identity or topological order is
  invalid`. The T003/T004 fixture put its canonical source graph digest into
  `NativeModelDescriptor::graphDigest`; the production YOLO catalog uses that
  field for the planning graph digest and separately pins the canonical source
  graph digest in `source.canonical_graph_digest`. Cold, refresh, and
  concurrency cases therefore failed before a verified Package was published;
  no cache or Runtime product result is inferred. Raw output is
  `.codex-tmp/spec185-b2/normal-preparation.log`.
- This is a C++ fixture identity boundary. The fix derives the planning graph
  digest from `inspectNativeOnnxSourceGraph` while retaining the canonical
  source identity in the catalog field, then requires static re-review and a
  fresh selector run.

## 2026-09-13 — Spec185 B2 v4 compile fixture syntax boundary

- The fresh `-j4` rebuild after the v19 static pass stopped before linking at
  `tests/unit-tests/di-preparation.t.cpp:74`: the `NativeAssemblyControl`
  aggregate initializer had one extra closing parenthesis. No selector ran
  from this binary. Raw output is `.codex-tmp/spec185-b2/normal-build-v4.log`.
- This is a test-fixture syntax boundary, not a production or protocol result.
  The correction removes the extra delimiter; the changed fixture must pass a
  new read-only static review and a fresh build before selectors are retried.

## 2026-09-13 — Spec185 B2 v5 preparation selector fixture boundary

- The v5 normal build linked successfully, but the preparation selector stopped
  in `PreparationRejectsWrongIdentityAndNonOnnxSource` when the unsupported-task
  case changed the adapter task list without recomputing the descriptor's
  planning graph identity. Native catalog validation therefore reported
  `graph snapshot identity or topological order is invalid` before reaching the
  intended capability check. Raw output is
  `.codex-tmp/spec185-b2/normal-preparation-v2.log`.
- This is a C++ fixture contract boundary. The corrected case keeps the valid
  descriptor/catalog identity and changes only the pinned request task (and its
  canonical requester digest), so production reaches the unsupported-capability
  branch. The fixture requires static re-review and a fresh selector run.

## 2026-09-13 — Spec185 B2 Runtime regression hang boundary

- The v5-linked binary passed the complete `Spec185Preparation` selector (14/14),
  the T004 Runtime selector (1/1), and the Core regression selector (35/35).
  A broader `Spec185Runtime` compatibility run entered
  `PrepareReportsSourceFailureAtPreparationBoundary` and remained blocked in a
  futex for more than two minutes after the first case passed. The process was
  terminated; no PASS or return code is inferred. Raw output is
  `.codex-tmp/spec185-b2/normal-runtime-regression-v1.log` and the termination
  marker is `.codex-tmp/spec185-b2/normal-runtime-regression-v1.rc`.
- This is a Runtime/fixture lifecycle boundary outside the already passing
  preparation selector. Before retrying the broad regression, reproduce the
  single test with a bounded timeout and inspect worker, close, drain, and
  preparation-failure ownership; preserve this run as evidence.

- A gdb thread snapshot of the isolated test showed the cache worker blocked
  in `User::prepare`'s `spec.cancelled` callback while `prepareSingle` still
  held the Runtime mutex through `spec.acquireCommit`; the caller waited on the
  job condition. This was a same-thread lock inversion, not a source-file or
  Core worker stall. The fix releases the commit guard before the cancellation
  probe, then requires a new static review and selector regression.

## 2026-09-13 — Spec185 B2 Runtime regression fixture identity boundary

- After the deadlock fix, the broad `Spec185Runtime` run completed all lifecycle
  cases until `PrepareSuccessUsesTheProductionRuntimeEntry`, whose unsupported
  capability subcase changed the adapter task list without recomputing the
  planning graph identity. The native catalog consequently rejected the
  descriptor before the intended capability branch. Raw output is
  `.codex-tmp/spec185-b2/normal-runtime-regression-v2.log`.
- This is a C++ Runtime fixture contract boundary. The fix keeps the valid
  descriptor/catalog graph and changes only the request task, allowing the
  production unsupported-capability check to run. A fresh static review and
  Runtime regression are required.

## 2026-09-13 — Spec185 B2 TSan synchronous error lifetime boundary

- The first fresh TSan preparation repetition exited 66 in
  `PreparationRejectsDetachedCatalogAndHonoursCancellation`: the worker
  destroyed the job-owned `std::runtime_error` while the main thread's
  `BOOST_CHECK_EXCEPTION` predicate still read `what()`. The second repetition
  happened to pass, but the diagnostic is a real exception-object lifetime
  race. Raw output is `.codex-tmp/spec185-b2/tsan-preparation-repeat1.log`.
- The failure is in synchronous `waitFor`, which directly rethrew
  `job->error` and allowed the last `exception_ptr` to disappear as the
  handle/state unwound. The fix copies the diagnostic text while the job lock
  protects the stored exception and throws an independent `std::runtime_error`;
  asynchronous completion payloads retain their own exception pointer. A new
  static review, normal rebuild and repeated TSan preparation selector are
  required.

## 2026-09-13 — Spec185 B3 normal selector fixture identity boundary

- The first fresh normal B3 build completed successfully for
  `spec185-prepared-request` and `spec185-core-operation` (103.99 seconds,
  `-j4`), but the prepared-request selector returned 201. Its first product
  boundary was `PreparedRequestCompletesThroughProvider`: the test-created
  authenticated grant client used the catalog recipe epoch `fixture-epoch`,
  while the Runtime's operator grant contract correctly used `epoch-1`; the
  native runtime rejected that identity mismatch before ACK planning. The
  same request failure caused later drain assertions to report false and is
  not yet an independent drain defect. Raw output is
  `.codex-tmp/spec185-b3/normal-runs/prepared-request-v1.log`; the successful
  build is `.codex-tmp/spec185-b3/normal-build-v2.log`.
- This is a C++ integration-fixture contract boundary. The fix derives the
  grant client's protection epoch from the operator `grant` section, then
  requires a fresh static review and selector run before assessing drain
  behavior.

## 2026-09-13 — Spec185 B3 stale-candidate rerun boundary

- A second prepared-request selector invocation returned 201 with only the
  same identity mismatch, but it ran against the v2 binary linked before the
  v18 fixture repair. It is preserved as `.codex-tmp/spec185-b3/normal-runs/
  prepared-request-v2.log` and is not a product result or a regression
  qualification. A fresh build is required before judging the repair.

## 2026-09-13 — Spec185 B3 Core Face fixture transport boundary

- The fresh v18/v19-linked normal selector closed the grant identity mismatch,
  then stopped at `PreparedRequestCompletesThroughProvider` when the Runtime's
  private Core `ServiceUser` raised `Failed to fetch public parameters after
  multiple attempts.` Its Face was not connected to the in-process Attribute
  Authority; `RuntimeTestAccess::bindProviderFixture` replaced the state user
  only after `Runtime::open` had already constructed that un-routable NAC-ABE
  owner. The same Core I/O failure left
  `RuntimeDrainAsyncIncludesNativeClientWork` without its callback before its
  bound wait. Raw output is
  `.codex-tmp/spec185-b3/normal-runs/prepared-request-v3.log`.
- This is a C++ test-transport ownership boundary. The repair requires the
  fixture binding to precede Core I/O and releases the unused production
  ServiceUser/Face before the first client starts the Core lifecycle; the
  real fixture ServiceUser remains the borrowed transport owner. A fresh
  static review, build, and selector run are required.

## 2026-09-13 — Spec185 B3 fixture Face teardown ordering boundary

- The v20 static review rejected its first transport-reset repair: resetting
  `CoreRuntimeOwner::face` after only `owner->serviceUser.reset()` left
  `RuntimeState::coreUser`, the old grant callbacks, and each frozen trust
  validator holding Face-bound state. That violated the ServiceUser/Face
  destruction order and could leave dangling callback targets. No new build
  or selector was run from v20. The review snapshot is
  `.codex-tmp/spec185-t006-review-v20`.
- The corrected repair releases old grants, core user, and model validators,
  and rejects injection while preparation or clients are active, before
  releasing the owner ServiceUser and Face. It requires static re-review and
  ASan-backed lifecycle validation.

## 2026-09-13 — Spec185 B3 eager NAC bootstrap classification boundary

- The v21-linked fresh selector cleared the dangling-Face risk but showed
  that allocation-only Runtime cases still became globally `ioFailed` when
  their unconnected Core ServiceUser exhausted NAC public-parameter retries.
  The first such failure was in `PreparedRequestsSharePackageButAllocateIndependentIds`;
  the run then cascaded into drain and fixture SIGSEGV failures. Raw output is
  `.codex-tmp/spec185-b3/normal-runs/prepared-request-v4.log`.
- This is a Core lifecycle classification boundary. The requester remains
  fail-closed while NAC is unready, but the stable bounded
  `Failed to fetch public parameters after multiple attempts` diagnostic is
  treated as recoverable bootstrap state and no longer poisons the Runtime
  `ioFailed` terminal signal. Unexpected Face exceptions still set `ioFailed`
  and notify active clients. A fresh static review, build, and selector run
  are required.

## 2026-09-13 — Spec185 B3 completion-subscription crash boundary

- The v22-linked normal selector passed the allocation-only, streaming, and
  default-drain cases, then aborted in
  `PreparedRequestCompletesThroughProvider` at the expected subscription-limit
  assertion (`di-prepared-request.t.cpp:738`). Instead of returning the
  documented `SUBSCRIPTION_LIMIT` error on the 65th completion subscription,
  the process hit a memory access violation. Raw output is
  `.codex-tmp/spec185-b3/normal-runs/prepared-request-v5.log`.
- This is a native completion-slot ownership/concurrency boundary. No
  provider result or qualification is counted; isolate the single test under
  a bounded debugger/sanitizer run before changing the slot implementation.

## 2026-09-13 — Spec185 B3 published assembly identity ordering boundary

- The first selector built after the completion-slot/lifecycle fixes still
  returned 201 in `PreparedRequestCompletesThroughProvider`: the Planner
  passed post-publication role metadata into `NativePlanSealer`, whose
  preflight correctly compared it with the original V3 proposal and rejected
  the changed manifest/recipe identity. Raw output is
  `.codex-tmp/spec185-b3/normal-runs/prepared-request-v6.log`; the stale
  candidate rerun is retained separately and is not qualification evidence.
- The repair keeps original prepared roles for the Sealer's placement
  preflight, then lets `bindPublishedRoles()` produce certified roles for the
  final Core assembly. v26 static review covered the change; a fresh build and
  runtime selector were required.

## 2026-09-13 — Spec185 B3 protection-epoch fixture boundary

- The selector in `.codex-tmp/spec185-b3/normal-runs/prepared-request-v7.log`
  was built before the v27 fixture edit and therefore remains a pre-v27
  stale run. It stopped at the generic `native sealed assembly differs from
  authenticated artifact context` boundary; the raw log does not prove that
  the later epoch repair was exercised and is not qualification evidence.
- The inspected fixture contract showed the cause: its catalog recipe used
  `fixture-epoch` while the Runtime grant contract used `epoch-1`. This was a
  test-fixture mismatch, not a reason to weaken the production security check.
  The fixture now uses the Runtime/grant epoch, while the wrong-epoch
  protected DataRef remains an explicit rejection case. v27 static review was
  required before the fresh v27 build and selector.

## 2026-09-13 — Spec185 B3 borrowed-Face drain boundary

- With the epoch aligned, the full native selector passed the Provider
  request, ACK/selection/grant/response checks, wrong-digest rejection,
  revocation failure, and both native drainAsync cases. The Provider fixture's
  final direct `Runtime::drain()` nevertheless returned false because its
  test-only `RuntimeTestAccess` binding borrows the environment Face and the
  terminal cleanup task is correctly queued on that external I/O owner; the
  test stopped pumping before drain. Raw output is
  `.codex-tmp/spec185-b3/normal-runs/prepared-request-v8.log`.
- The fixture now runs the bounded Runtime drain in a future while
  `environment.pumpUntil()` drives the borrowed Face. Production Runtime
  drain semantics and own-Face lifecycle tests are unchanged. v28 static
  review and a fresh selector are required.

## 2026-09-13 — Spec185 B3 sanitizer timing and leak boundary

- The same-ABI ASan/UBSan fast tree configured with `-O0 -g0` built all 109
  objects successfully in `.codex-tmp/spec185-b3/asan-ubsan-fast-build-v1.log`.
  Its first prepared-request selector stopped in
  `PreparedRequestCompletesThroughProvider` at the cooperative extension
  deadline check because sanitizer overhead exceeded the fixture's 100 ms
  policy budget.  The selector returned 134 and LeakSanitizer then reported
  12,503 bytes in 27 allocations, primarily ndn-cxx SegmentFetcher/Face and
  scheduler objects.  Raw output is
  `.codex-tmp/spec185-b3/asan-ubsan-fast-runs/prepared-request-r1.log`.
- This is an unqualified dynamic failure, not a license to suppress
  LeakSanitizer or weaken the production deadline.  The fixture timing branch
  is being made portable across GCC and Clang, and an independent strict
  1 ms C++ policy-budget counterexample is required before the prepared
  selector is retried.  The leak report remains an open lifecycle boundary
  until a clean sanitizer run proves otherwise.

## 2026-09-13 — Spec185 B3 drain lock-order and notifier publication boundary

- The B3 composition review rejected v34's first lock-order repair with a
  P1: the Core drain predicate held the Core mutex while taking the DI state
  mutex, while `makeRuntimeClient()` held the DI mutex while installing the
  Core notifier.  The replacement now publishes an immutable atomic client
  snapshot, so the Core predicate no longer takes the DI mutex.
- The same review found a second P1 window in v35: publishing a client before
  installing its notifier allowed a concurrent terminal request to miss the
  outer drain wakeup.  The notifier is now installed before the client enters
  the map or snapshot; the C++ regression registers a multi-client drain
  before cancelling either request.  v35 was static-fail and no build or
  selector result from it is qualification evidence.  A fresh static review,
  composition review, build, and runtime selector are required.

## 2026-09-13 — Spec185 B3 normal drain regression after snapshot repair

- The normal B3 rebuild after the v37 composition pass succeeded, and
  `Spec185ExtensionRegistry` passed 10/10.  The prepared-request selector
  reached the Provider path and the new active two-client drain case, but the
  existing `RuntimeDrainAsyncIncludesNativeClientWork` waiter did not complete
  within its two-second bound; the selector returned 201.  Raw output is
  `.codex-tmp/spec185-b3/normal-runs/prepared-request-v10.log`.
- This is a native drain/lifecycle regression boundary introduced after the
  transactional snapshot/notifier repair.  The previous v9 pass is stale for
  this source.  The failing selector is being isolated before any sanitizer
  retry; no result from v10 counts toward B3 qualification.

## 2026-09-13 — Spec185 B3 sanitizer fixture NAC cleanup boundary

- After the normal selector was rerun successfully, the ASan/UBSan prepared
  selector r2 exercised all 12 C++ cases and reported `*** No errors detected`
  from Boost.Test, but LeakSanitizer aborted with 12,503 bytes in 27
  allocations.  The dominant live chain begins at
  `ServiceUser::refreshNacDkeyForControllerStatus` during the revoked-status
  case and ends in ndn-cxx `SegmentFetcher`/Face scheduler objects.  Raw output
  is `.codex-tmp/spec185-b3/asan-ubsan-fast-runs/prepared-request-r2.log`.
- This remains a dynamic UNQUALIFIED result.  The fixture now pumps the real
  Attribute Authority and User Faces until the post-revocation NAC DKEY is
  ready before releasing the borrowed transport; sanitizer r3 is required.
  LeakSanitizer remains enabled and no external allocation is suppressed.
### 2026-09-13 Spec185 B3 repeated selector resource-interference boundary

After the first clean B3 normal and ASan/UBSan runs, a second repetition was
started as four selector processes at once.  The normal prepared selector
stopped at the `RuntimeDrainAsyncIncludesNativeClientWork` 5-second wait;
the ASan prepared selector timed out in the provider completion case, then
reported an ASan `DEADLYSIGNAL`/LSan failure while another selector was still
running.  The extension selectors passed.  These runs are preserved as
`.codex-tmp/spec185-b3/normal-runs/prepared-request-v15.log`,
`.codex-tmp/spec185-b3/normal-runs/extension-registry-v4.log`,
`.codex-tmp/spec185-b3/asan-ubsan-fast-runs/prepared-request-r5.log`, and
`.codex-tmp/spec185-b3/asan-ubsan-fast-runs/extension-registry-r4.log` and
remain `UNQUALIFIED`; concurrent selectors are not a valid repetition on
this host.  The changed gate is sequential, isolated selector runs with no
competing build or test process; no source change is made from this boundary.

### 2026-09-13 Spec185 B4 first compile boundary

The B4 normal incremental build reached the test translation unit after the
native conversation sources compiled, then stopped in
`tests/integration-tests/di-prepared-request.t.cpp`.  The new conversation
fixture used the unqualified `StreamFinishReason` type, and two checkpoint
byte-vector assertions used `BOOST_CHECK_EQUAL`, whose diagnostic printer has
no `operator<<` for `std::vector<uint8_t>`.  The complete raw output is
`.codex-tmp/spec185-b4/normal-build-v1.log` with `BUILD_RC=1`; this is a test
compile boundary, not a native runtime or protocol result.  The repair uses
the fully qualified Core enum and boolean vector equality.  Affected test
source requires a fresh static review before the next build.

### 2026-09-13 Spec185 B4 second compile boundary

After the test-only repair passed static review, the next `-j4` build compiled
the test translation units and stopped at `Conversation.cpp`: `DiError` was
only forward-declared through `PreparedModel.hpp`, but the conversation
implementation constructs and catches that type.  Raw output is
`.codex-tmp/spec185-b4/normal-build-v2.log` with `BUILD_RC=1` (39 seconds).
This is a production translation boundary, not a link or runtime result.  The
repair adds the owning `Runtime.hpp` definition include; the affected
production range requires a fresh static review before retry.

### 2026-09-13 Spec185 B4 selector and generation-contract boundaries

The first post-build selector command used a comma-separated Boost.Test filter
and returned `200` with “no test cases matching filter”.  Raw output is
`.codex-tmp/spec185-b4/normal-runs/conversation-r1.log`; this is a selector
syntax boundary and is not a product result.  The corrected exact selector
entered the native request chain and returned `201` after five of six early
assertions passed.  The first request stopped in native planning with
`NATIVE_REQUEST_STAGE_FAILED`: `generation role omits a sealed state input`.
Raw output is `.codex-tmp/spec185-b4/normal-runs/conversation-r2.log`.

The failure exposed an invalid fixture contract: the conversation test used the
YOLO source graph while its `TOKEN_STREAMING` runtime defaults required Qwen
state tensors (`attention_kv_in`, `recurrent_state_in`, and
`convolution_state_in`).  Production planning correctly rejected the role;
weakening that check would hide a real missing model boundary.  The fixture is
being switched to the existing source-bound Qwen native-config ONNX/catalog
path, while the ordinary streaming cancellation probes retain the YOLO fixture.
No runtime PASS is counted until the repaired fixture is reviewed and rerun.

### 2026-09-13 Spec185 B4 protected-grant publication boundary

The repaired Qwen fixture passed the incremental normal build, but the exact
conversation selector returned `201` after reaching the authenticated request
stage: `DI_PROTECTED_GRANT_REJECTED: published manifest differs from authorized
source`.  Raw output is `.codex-tmp/spec185-b4/normal-runs/conversation-r3.log`.
The T007 grant fixture still authorized the legacy YOLO `fixture-profile` while
the new Qwen catalog advertised a different artifact profile.  This is a
fixture identity mismatch; production grant verification correctly rejected the
unbound publication.  The Qwen catalog now keeps its source/model identities
but uses the explicitly authorized shared fixture profile.  Affected-range
static review and a fresh selector are required; no B4 runtime PASS is counted.

### 2026-09-13 Spec185 B4 Qwen payload boundary

After the grant identity repair, the exact conversation selector reached native
request preparation but returned `201`: `native task payload exceeds the adapter
byte bound`.  Raw output is `.codex-tmp/spec185-b4/normal-runs/conversation-r4.log`.
The Qwen fixture retained the compact YOLO `max_payload_bytes=32` despite its
generation envelope carrying tokenizer and state metadata.  Production input
validation correctly rejected the oversized envelope.  The Qwen catalog bound
is now aligned with the maintained native-config value of 4096; the YOLO probe
limit remains unchanged.  Affected-range static review and a fresh build and
selector are required.

### 2026-09-13 Spec185 B4 conversation append-prefix boundary

With the Qwen source, grant profile, and payload bound repaired, the first
turn committed and checkpoint export/import assertions passed.  The second
turn returned `201` at `NativeConversationCoordinator::beginTurn` with
`conversation append prefix mismatch`; raw output is
`.codex-tmp/spec185-b4/normal-runs/conversation-r5.log`.  The public
`Conversation::makeContinuation` carried the previous committed token list
unchanged, while the append contract requires a strictly longer canonical
prefix before opening the next turn.  This is a real API/fixture semantic
boundary, not a provider transport result.  No B4 PASS is counted; the next
repair must bind the new input's canonical token prefix through the native
conversation/tokenizer contract rather than weakening the coordinator check.

### 2026-09-13 Spec185 B4 token provenance review boundary

The first prefix repair exposed a P1 in the official static review: a public
`RequestOptions.canonicalTokenIds` vector allowed a caller to forge a
parent-extending lineage.  The repair removed that field and made the verified
native adapter derive the current input suffix.  The next static review found
the catalog constructor moved the first adapter encoder before checking its
presence, so the moved-from function was reported as inconsistent and Qwen
preparation could never reach the conversation path.  The code now snapshots
the presence bit before moving the function.  No compile/link/runtime result
is counted until the affected range is reviewed again.

2026-09-13 Spec185 B4 runtime boundary: after the successful normal `-j4`
compile-link, selector
`Spec185PreparedRequest/PreparedConversationCommitsTwoNativeTurns` reached the
checkpoint assertion but returned `RC=201` with
`final payload disagrees with accepted generation` (`.codex-tmp/spec185-b4/normal-runs/conversation-r6.log`).
The first boundary is the accepted-generation/final-payload binding in the
conversation fixture or production commit path; this is not a protocol or
qualification PASS.  Preserve the run and inspect the accepted result against
the final payload before retrying.

2026-09-13 Spec185 B4 repeated-runtime boundary: after the fixture token
repair, the first normal conversation selector passed, but the required
repeat (`.codex-tmp/spec185-b4/normal-runs/conversation-r10.log`) timed out at
the restored third turn's bounded 8-second `result()` wait with
`native result wait timed out`.  The run reported no new protocol mismatch;
preserve it as a scheduling/observation-budget failure and widen the bounded
test budget before retrying.

2026-09-13 Spec185 B4 sanitizer-runtime boundary: the ASan/UBSan conversation
selector attempts (`.codex-tmp/spec185-b4/asan-runs/conversation-r1.log` and
`conversation-r2.log`) reached the first committed-turn result observation
and returned `RC=201` with `native result wait timed out`.  The corresponding
sanitizer build (`asan-ubsan-build-v5.meta`) was `BUILD_RC=0`, and the raw
selector logs contain no ASan, UBSan, or LSan report.  The first boundary is
therefore the bounded result observation under the unoptimised sanitizer
runtime, not a detected memory error; preserve both runs and retry only
after the fixture's sanitizer-specific 60-second request/45-second result
budget has passed static review.

2026-09-13 Spec185 B4 sanitizer liveness boundary: after the extended
180s/120s budget review and sanitizer build v7, conversation selectors r7 and
r8 both returned `RC=201` from the first-turn result observation at 122s and
181s respectively (`.codex-tmp/spec185-b4/asan-runs/conversation-r7.log` and
`conversation-r8.log`).  The sanitizer API selectors passed and neither
conversation log contains an ASan, UBSan, or LSan report.  Preserve this as an
unresolved sanitizer scheduling/liveness boundary; normal conversation r23/r24
passing does not qualify B4, and no further budget increase should be treated
as a fix until the first stalled production/fixture wait is identified.

2026-09-13 Spec185 B4 fixture pump diagnosis: the sanitizer conversation
timeouts were traced to the C++ test harness rather than a sanitizer report.
`NdnsfIntegrationEnvironment::pumpUntil()` processes at most 200 five-ms face
rounds (about four seconds); the test then blocks on `future.get()` without
continuing DummyFace event processing.  Slow ASan requests therefore cannot
advance after the first pump chunk and eventually hit the bounded result or
request timeout.  The planned repair is a C++ helper that repeats pump chunks
until the future is ready or the request deadline expires, followed by static
review and sanitizer/normal reruns.

2026-09-13 Spec185 B4 normal-repeat boundary: after the sanitizer-budget
static pass and normal v10 rebuild, conversation repeat r15 passed but r16
returned `RC=201` at the first-turn result observation with
`native result wait timed out` (`.codex-tmp/spec185-b4/normal-runs/conversation-r16.log`).
The preceding five assertions passed and no protocol or sanitizer diagnostic
was reported.  Preserve the run as the same host scheduling/observation
boundary seen in r10 and sanitizer r1-r2; widen the fixture's bounded normal
request/result budget to 60s/45s and require another static review before the
next build and repeat.

2026-09-13 Spec185 B4 extended-observation boundary: after the uniform 60s/45s
budget static pass and normal v11 build, conversation r19 reached the 45s
first-turn result observation and returned `RC=201` with
`native result wait timed out`; the immediate r20 repeat passed.  No protocol,
ownership, or sanitizer diagnostic was emitted.  Preserve both runs and raise
the explicit bounded fixture budget to 180s request/120s result, then require
another static review and rebuild before counting repeated runtime PASS.

2026-09-13 Spec185 B4 fixture-helper compile boundary: the first build after
adding the repeated event-pump helper stopped in test translation
(`.codex-tmp/spec185-b4/normal-build-v13.log`, `BUILD_RC=1`) because the
helper used unqualified `NdnsfIntegrationEnvironment`; the declared type is
`ndn_service_framework::test::NdnsfIntegrationEnvironment`.  This is a
test-only compile error; no native production or runtime result is counted.

2026-09-13 Spec185 B4 sanitizer fixture crash: after normal v14 and sanitizer
v9 builds, conversation selectors r9/r10 both returned `RC=1` with an
AddressSanitizer `DEADLYSIGNAL` write in
`NdnsfIntegrationEnvironment::pumpUntil` (`.codex-tmp/spec185-b4/asan-conversation-r9.log`,
`asan-conversation-r10.log`).  A gdb run with `handle_segv=0` preserved the
main-thread stack at `pumpUntil` and showed no production request/coordinator
frame; worker threads were idle in their queues.  The register/disassembly
record identifies the fault at the sanitizer stack-frame cleanup write.  No
UBSan or LSan report was emitted.  This is an unresolved C++ fixture memory
failure, so it is not a runtime or qualification PASS; the next repair keeps
the future query outside the fixture pump callback and requires static review
plus normal/sanitizer reruns.

2026-09-13 11:46 -05:00 Spec185 B4 sanitizer thread-exception retry boundary:
after the C++ competing-request exception was moved into a joined thread and
passed static review, strict conversation run
`conversation-r21.log` still returned `RC=134` with
`*** stack smashing detected ***` at the second-request assertion.  The
`detect_stack_use_after_return=0` repeat `conversation-r22-no-stack-uarr.log`
returned the same boundary.  GDB broke `__stack_chk_fail` in the main-thread
fixture `NdnsfIntegrationEnvironment::pumpUntil`; no production requester or
coordinator frame was present.  Preserve both runs as an unresolved fixture
pump/exception interaction, not a protocol or qualification result.  The next
repair removes promise/future bookkeeping from that probe, then requires
affected static review, a shared rebuild, and strict sanitizer repeats.

2026-09-13 11:46 -05:00 Spec185 B4 repair static gate: the atomic-result
competing-request probe passed the official read-only `review-agent` from
immutable snapshot `.codex-tmp/spec185-b4-after-atomic-exception-static-v1`
with no P0/P1/P2/P3 findings.  This records a static repair only; compile-link
and strict runtime lanes remain unobserved and T007/T008 stay `PARTIAL`.

2026-09-13 11:46 -05:00 Spec185 B4 compile boundary: the first normal build
after the atomic-result repair stopped before compilation with Waf
`The project was not configured: run "waf configure" first!` (`BUILD_RC=1`).
Raw output is `.codex-tmp/spec185-b4/normal-build-v18.log` and metadata is
`.codex-tmp/spec185-b4/normal-build-v18.meta`; no source or runtime result is
counted.  Reconfigure the affected target with the verified system toolchain
before retrying.

2026-09-13 11:54 -05:00 Spec185 B4 sanitizer atomic-probe boundary: after
static review, normal build v19 and conversation/API repeats passed, and ASan/
UBSan build v13 completed.  Strict conversation `conversation-r23.log` still
returned `RC=134` with `*** stack smashing detected ***` near the atomic
concurrent-request assertion; removing promise/future bookkeeping did not
change the boundary, and no production requester/coordinator diagnostic was
reported.  Preserve the run as unresolved C++ fixture/concurrency behavior,
not a protocol or qualification result; reduce the probe further before the
next strict retry.

2026-09-13 12:02 -05:00 Spec185 B4 sanitizer repeat boundary: strict
conversation r24 passed all 20 assertions, but immediate repeat r25 returned
`RC=134` with the same stack-smash boundary at the atomic concurrent-request
assertion.  The pair is intermittent and cannot qualify B4.  Preserve both
raw logs; the next controlled isolation keeps the active-turn negative case
in the caller thread and requires static review, rebuild, and strict repeats.

2026-09-13 12:08 -05:00 Spec185 B4 direct-exception static gate: the controlled
same-thread active-turn rejection probe passed the official read-only
`review-agent` from immutable snapshot
`.codex-tmp/spec185-b4-after-direct-exception-static-v1` with no P0/P1/P2/P3
findings.  The reviewer confirmed the production mutex, completion callback,
and close/cancel lifetime remain unchanged.  This repair intentionally does
not observe true cross-thread competition; compile-link and strict runtime
lanes remain unobserved until the shared rebuild and selector repeats.

2026-09-13 12:32 -05:00 Spec185 B4 direct-exception runtime boundary: normal
build v20 on the existing `.lock-spec185-b0c-normal` tree completed with
`BUILD_RC=0` in `25.611s` at `-j4`; conversation runs r39 and r40 both passed
all 20 assertions.  ASan/UBSan + LSan build v14 completed with `BUILD_RC=0` in
`40.614s`, but strict conversation r26 returned `RC=134` with stack-smash and
r27 returned `RC=1` with nested `AddressSanitizer: DEADLYSIGNAL`; neither
included a production requester/coordinator frame.  The API rejection
selector passed sanitized runs r19 and r20.  Preserve the conversation failures
as an unresolved fixture/dependency exception boundary; T007/T008 remain
`PARTIAL` and no B4 qualification PASS is inferred.

2026-09-13 13:26 -05:00 Spec185 B4 deferred-bridge static gate: to isolate the
ndn-svs/fixture synchronous re-entry boundary, the test fixture added an
opt-in `BootstrapProfile::deferBridgeDelivery` mode; only the Spec185
conversation profile enables it and the default remains inline.  Repair-only
snapshot `.codex-tmp/spec185-b4-after-deferred-bridge-static-v6` passed the
official read-only `review-agent` with `STATIC_PASS` and no P0/P1/P2/P3
findings.  Base, DIFF SHA, and PATHS SHA are recorded in
`specs/185-prepared-model-runtime/evidence/b4-conversation.md`.  Compile-link
and runtime lanes remain unobserved pending the shared rebuild.

2026-09-13 13:58 -05:00 Spec185 B4 deferred-bridge runtime boundary: normal
build v21 completed with `BUILD_RC=0` in `31.529s`; conversation r41/r42 both
passed all 20 assertions.  ASan/UBSan + LSan build v15 completed with
`BUILD_RC=0` in `45.586s`, but strict conversation r31 still returned
`RC=134` with stack-smash at the active-turn assertion; no production
requester/coordinator frame was reported.  The opt-in queued bridge did not
change the sanitizer boundary.  Preserve this as an unqualified fixture/
exception path; the next diagnostic skips only that assertion and is not a
qualification run.

2026-09-13 14:20 -05:00 Spec185 B4 exception-path diagnosis: a repair-reviewed
diagnostic switch temporarily skipped only the active-turn exception probe.
Normal build v22 and ASan/UBSan build v16 completed; with
`SPEC185_SKIP_CONVERSATION_BUSY_PROBE=1`, the strict sanitizer conversation
selector completed all subsequent turns, checkpoint recovery/export, close,
and drain with `RC=0` and no sanitizer errors.  The switch was removed and the
mandatory `CONVERSATION_TURN_IN_PROGRESS` assertion restored.  This isolates
the unresolved boundary to the real C++ exception path under the current
sanitizer/dependency combination; the diagnostic run is not qualification
evidence and T007/T008 remain `PARTIAL`.

2026-09-13 13:01 -05:00 Spec185 B4 closure: final composition snapshot
`.codex-tmp/spec185-b4-composition-v3` passed the official read-only
`review-agent` with `B4_COMPOSITION_PASS / STATIC_PASS`.  The post-composition
normal v26 and ASan/UBSan+LSan v19 builds succeeded; normal conversation r49/r50,
API r51/r52, strict sanitizer conversation r40/r41, and API r42/r43 all returned
`RC=0` with no test or sanitizer errors.  An earlier normal selector used the
nonexistent `build-spec185-b0-normal` path and returned `RC=127`; rerunning from
the verified `build-spec185-b0c-normal` directory passed, so that boundary is a
command-path failure rather than a product runtime failure.  B4 evidence is
closed and T007/T008 are `PASS`; cross-thread scheduler pressure, Python, and
B5-B9 remain unverified.

## 2026-09-13 Spec185 B5 T009 static boundary

官方 `review-agent` 对 `.codex-tmp/spec185-b5-t009-static-v2` 返回 `STATIC_FAIL`。首个边界是 Provider facade 尚未形成真实 provider runtime：缺少 Core/证书/IO 初始化和 authenticated post-Selection assembly factory；同时角色允许集、drain 屏障、Runtime provider 配置一致性与 C++ oracle 不完整。未运行 build/runtime；不得将静态或本地注册 fixture 记为 T009 PASS。修复范围见 `specs/185-prepared-model-runtime/evidence/b5-provider.md`。

## 2026-09-13 Spec185 B5 T009 repair review boundary

官方 `review-agent` 对 T009 修复快照 v3 复审仍返回 `STATIC_FAIL`。首个生命周期边界是 Provider drain 超时后留下 joinable IO 线程且 `stop()` 提前返回；同时 Runtime 吞掉 drain 失败。生产边界仍缺 Controller permission/bootstrap 复用和 manifest 身份验证，fixture 关键 oracle 未覆盖。未运行 build/runtime；详见 `specs/185-prepared-model-runtime/evidence/b5-provider.md`。

## 2026-09-13 Spec185 B5 T009 repair review boundary v4

官方 `review-agent` 对 T009 修复快照 v4 返回 `STATIC_FAIL`。首个产品边界是 protected runtime factory 未接线，随后为 provider drain/async 生命周期、并发 stopIo join 竞态和 controller certificate 测试身份隔离；C++ authenticated Selection/assembly oracle 仍不足。未运行 build/runtime；详见 `specs/185-prepared-model-runtime/evidence/b5-provider.md`。

## 2026-09-13 Spec185 B5 T009 repair review boundary v5

官方 `review-agent` 对 T009 修复快照 v5 返回 `STATIC_FAIL`。新增首个边界是 IO 启动后 Provider::serve 未切换到 Face 线程；同时 Runtime::close 仍可能同步等待 300 秒，fixture 缺少 authenticated Selection/assembly/counter oracle 并丢弃 drainAsync Subscription。未运行 build/runtime；详见 `specs/185-prepared-model-runtime/evidence/b5-provider.md`。

## 2026-09-13 Spec185 B5 T009 repair review boundary v6

官方 `review-agent` 对 T009 修复快照 v6 返回 `STATIC_FAIL`。首个编译边界是 ProviderCounters API 错位；生命周期边界是 Face dispatch 与 stop 的永久等待、registration detach 未纳入 drain barrier，以及 start/stop admission race。正向 authenticated Selection/assembly oracle 仍缺失。未运行 build/runtime；详见 `specs/185-prepared-model-runtime/evidence/b5-provider.md`。

## 2026-09-13 Spec185 B5 T009 repair review boundary v7

官方 `review-agent` 对 T009 修复快照 v7 仍返回 `STATIC_FAIL`。v6 的编译和 Face-stop 生命周期缺陷已修复；当前首个剩余边界是 `serveMutex` 跨 Face condition wait，随后是 serve/start/stop 的稳定 `DiError` 映射，以及缺少 authenticated Selection→grant/assembly/runner→Response、wrong provider/epoch/grant 和正向 counter 的 C++ oracle。未运行 build/runtime；详见 `specs/185-prepared-model-runtime/evidence/b5-provider.md`。

## 2026-09-13 Spec185 B5 T009 repair review boundary v9

官方 `review-agent` 对 T009 修复快照 v9 仍返回 `STATIC_FAIL`。首个编译边界是 ProtectedRuntime factory 未捕获 `nowMs`；随后是 `ProviderConfig::Impl` 的匿名命名空间/私有访问问题。另有 wrong provider/epoch/grant 未经过 Provider ingress 的反例路径，以及 runner factory/protected factory 初始化异常未纳入稳定 `DiError` 映射。未运行 build/runtime；详见 `specs/185-prepared-model-runtime/evidence/b5-provider.md`。

## 2026-09-13 Spec185 B5 T009 repair review boundary v10

官方 `review-agent` 对 T009 修复快照 v10 返回 `STATIC_FAIL`。Provider-only Runtime 在 Provider drain 失败后仍保持 `Open`；正向 fixture 仍走 test preparation/runner factory，未覆盖真实 `NativeCanonicalOnnxAssembler` source-fetch/assembly 计数。未运行 build/runtime；详见 `specs/185-prepared-model-runtime/evidence/b5-provider.md`。

## 2026-09-13 Spec185 B5 T009 repair review pass v11

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t009-static-v11` 返回 `STATIC_PASS`，无 P0/P1/P2/P3。Runtime provider-only drain 在 Provider 调用前进入 `Closing`；C++ 正向 oracle 直接调用生产 `NativeCanonicalOnnxAssembler`，执行 canonical root/source fetch 与 OA02 assembly，并检查 fetch 计数。尚未构建或运行；动态端到端结果待 B5 组合门后取得，详见 `specs/185-prepared-model-runtime/evidence/b5-provider.md`。

## 2026-09-13 Spec185 B5 T010 static boundary v12

官方 `review-agent` 对 `.codex-tmp/spec185-b5-t010-static-v12` 返回 `STATIC_FAIL`。首个控制性缺陷是 single-flight creator 取消直接取消共享 job，仍有效的其他 waiter 会被误伤；另有完整 request/grant projection 与 plaintext runner path 进入 cache、source identity key/生产 cold-hit oracle/异常安全记账不足，以及内部 cache header 被 broad DI glob 安装。未运行 build/runtime；详见 `specs/185-prepared-model-runtime/evidence/b5-provider.md`。

## 2026-09-13 Spec185 B5 T010 static boundary v14

官方 `review-agent` 对 `.codex-tmp/spec185-b5-t010-static-v14` 返回 `STATIC_FAIL`，未构建/未运行。canonical root/source identity 和内部 header 安装边界已接线，但受保护 Provider 路径仍绕过 artifact cache；creator 作为唯一 waiter 时不会触发 last-waiter cancellation；publish 的 eviction/预算记账没有异常原子性；`maxArtifactBytes` 未覆盖并发 assembly 暂存与 template 内存。五 lane、快照身份和修复要求见 `specs/185-prepared-model-runtime/evidence/b5-provider.md`。T010 保持 `PARTIAL`，不能将任何 compile-link 或 runtime-test 结果外推为通过。

## 2026-09-13 Spec185 B5 T010 static boundary v17

官方 `review-agent` 对 `.codex-tmp/spec185-b5-t010-static-v17` 返回 `STATIC_FAIL`，未构建/未运行。protected ciphertext cache、每请求明文 staging、异常回滚和 reservation/template 计费已接线，但已取消 job 仍可被新请求加入，admission 在可驱逐 LRU 存在时提前拒绝，cache hit 未重验当前 assembled 上限，cache API 未校验 grant identity；另有 runner 字节溢出和 `stop() noexcept` 的 `exception_ptr` 分配边界。快照还遗漏未跟踪的 C++ fixture，下一次必须补入完整 diff。见 `specs/185-prepared-model-runtime/evidence/b5-provider.md`。T010 保持 `PARTIAL`。

## 2026-09-13 Spec185 B5 T010 static boundary v22

官方 `review-agent` 对 `.codex-tmp/spec185-b5-t010-static-v22` 返回 `STATIC_PASS`，无 P0/P1/P2/P3，未构建/未运行。审查确认 protected cache、取消代际、grant/provider/epoch exact-key、当前 assembled budget、assembly/template reservation、两阶段 LRU victim 预选和异常原子性静态闭合；mixed pinned/unpinned 反例、Provider 默认 protected 端到端及 compile-link/runtime/sanitizer 仍未观察。见 `specs/185-prepared-model-runtime/evidence/b5-provider.md`。T010 保持 `PARTIAL`，等待 B5 组合及批末验收。

## 2026-09-13 Spec185 B5 normal compile boundary v1

批次组合门后首次 `-j4` 共享构建仅编译 `spec185-provider-assembly`，在 `tests/integration-tests/di-prepared-provider.t.cpp:658`、`:723` 发现临时 `ndn::Buffer` 无法绑定 `RequestMessage::setPayload(ndn::Buffer&, size_t)` 的 non-const lvalue reference，返回 `rc=1`；生产 Provider/cache 尚未进入链接。原始输出见 `.codex-tmp/spec185-b5/normal-build-v1.log`，已改为具名可变 Buffer，待 v23 静态复审后重试。

## 2026-09-13 Spec185 B5 T010 repair review pass v23

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v23` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；两处具名可变 Buffer 修复符合 `RequestMessage::setPayload(ndn::Buffer&, size_t)` 契约，五 lane 无控制性缺陷。compile-link/runtime-test/sanitizer 仍未观测，T010 保持 `PARTIAL`，重试 B5 共享构建。

## 2026-09-13 Spec185 B5 normal compile boundary v2

v23 复审后第二次 `-j4` 共享构建仅编译 `spec185-provider-assembly`，在 `ProviderArtifactCache.cpp:166` 发现匿名 `makeLease` 无权调用 `ProviderArtifactLease` private constructor，并在 `Provider.cpp:1203` 发现缺少 `NativeRunnerPreparation.hpp` 声明，返回 `rc=1`；生产链接和运行尚未开始。原始输出见 `.codex-tmp/spec185-b5/normal-build-v2.log`，已将 helper 移入 `ProviderArtifactCache` 成员并补 include，待新快照静态复审后重试。

## 2026-09-13 Spec185 B5 T010 repair review pass v24

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v24` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；lease helper 权限、runner preparation include 与 v23 fixture 修复均闭合，五 lane 无新增控制性缺陷。compile-link/runtime-test/sanitizer 仍未观测，T010 保持 `PARTIAL`，重试 B5 共享构建。

## 2026-09-13 Spec185 B5 normal compile pass v3

v24 复审后既有 `build-spec185-b0c-normal` 以系统优先 PATH、`-j4` 仅构建 `spec185-provider-assembly` 成功，耗时 `59.408s`，候选 SHA256 `c70c7855a8d506c6376b9b03ac3eeb78a9070935566aedeb77fefb79bf512c79`；compile-link 已通过，runtime-test/sanitizer 尚未执行。原始输出见 `.codex-tmp/spec185-b5/normal-build-v3.log`。

## 2026-09-13 Spec185 B5 selector invocation boundary v1

首次 selector 枚举误传 `--list_content=tests`，Boost.Test 返回参数错误 `rc=200`，未执行测试；原始输出见 `.codex-tmp/spec185-b5/selector-list-v1.log`。这是 harness 参数边界，随后改用无值 `--list_content`，不计入产品行为结果。

## 2026-09-13 Spec185 B5 normal runtime boundary v1

候选 `spec185-provider-assembly` 完整 selector 返回 `rc=201`，8 个用例中 3 通过、5 失败。首个产品边界是 authenticated projection fixture 缺少完整 execution/dataflow/device binding；其次 assembler oracle 未找到同树 `DI_NativeOnnxAssemblyWorker`，cache pinned-entry 反例实际启动第三次 build。原始输出见 `.codex-tmp/spec185-b5/normal-run-v1/output.log`。失败已定位，修复后必须静态复审、重建并重跑，T009/T010 保持 `PARTIAL`。

## 2026-09-13 Spec185 B5 T010 repair review pass v25

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v25` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；projection binding、worker 路径和 maxArtifactEntries entry-admission 修复均通过五 lane 静态复核。compile-link/runtime-test/sanitizer 仍未观测，T010 保持 `PARTIAL`，重试 B5 构建。

## 2026-09-13 Spec185 B5 normal compile pass v4

v25 复审后 `spec185-provider-assembly` 以既有 `-j4` 构建成功，耗时 `18.793s`，候选 SHA256 `91c92e422b4870c039bcc0416114c0d5a8847875a6d39ee776d127ec750758d6`；compile-link PASS，唯一新增 warning 为 `ProviderArtifactCache.cpp:537` 未使用变量，已安排清理后复审，runtime-test/sanitizer 尚未执行。

## 2026-09-13 Spec185 B5 T010 repair review pass v26

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v26` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；仅删除未使用局部变量，五 lane 无回退。runtime-test/sanitizer/压力并发仍未观测，T010 保持 `PARTIAL`，重建后重跑 selector。

## 2026-09-13 Spec185 B5 normal compile pass v5

v26 复审后 `spec185-provider-assembly` 以既有 `-j4` 成功，耗时 `9.594s`，候选 SHA256 `00b7cc48c9fa9830ab6755cf35cc4c23c32942f3ea8350a486488622444f36b9`，无新增 warning；compile-link PASS，runtime-test/sanitizer 尚未执行。原始输出见 `.codex-tmp/spec185-b5/normal-build-v5.log`。

## 2026-09-13 Spec185 B5 normal runtime boundary v2

候选完整 `Spec185ProviderAssembly` selector 返回 `rc=201`，4/8 通过、4/8 失败；cache pin/eviction 与 protected cache 已通过，剩余为 `onnxruntime` device binding 与 V3 校验不一致，以及 OA02 fixture 伪 recipe digest 被 worker 正确拒绝为 `DI_NATIVE_ONNX_RECIPE`。原始输出见 `.codex-tmp/spec185-b5/normal-run-v2/output.log`，修复后必须静态复审、重建并重跑。

## 2026-09-13 Spec185 B5 T010 repair review pass v27

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v27` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；assembler/provider projection 的 adapterVersion、canonical recipe digest、SINGLE_DEVICE/cpu binding 修复闭合，五 lane 无回退。runtime-test/sanitizer/压力并发仍未观测，T010 保持 `PARTIAL`，重建重跑。

## 2026-09-13 Spec185 B5 normal compile pass v6

v27 复审后 `spec185-provider-assembly` 以既有 `-j4` 成功，耗时 `21.696s`，候选 SHA256 `1792d5d2d212e0dbd43347cffe240546c7694584e395db1f199c5457676bf62a`，无新增 warning；compile-link PASS，runtime-test/sanitizer 尚未执行。原始输出见 `.codex-tmp/spec185-b5/normal-build-v6.log`。

## 2026-09-13 Spec185 B5 composition boundary

官方 `review-agent` 对冻结组合快照 `.codex-tmp/spec185-b5-composition-v1` 返回 `B5_COMPOSITION_PASS`，无 P0/P1/P2/P3；批次增长停止于 T009/T010，组合静态覆盖已闭合，可进入共享 compile-link/runtime/sanitizer 验证。默认 Provider protected assembler→cache→decrypt/staging→runner、compile-link、runtime-test 和 sanitizer 仍未观察，不能据此将 T009/T010 记为完成。详见 [b5-provider](evidence/b5-provider.md)。

## 2026-09-13 Spec185 B5 T010 repair review pass v28

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v28` 返回 `T010_STATIC_PASS`，无 P0/P1/P2/P3；fixture 仅以 `RequestService` response callback 作为完成出口，ACK publication 处理保持不变，正向 Selection/assignment/Provider/runner/Response 与三类拒绝路径无静态回退。compile-link/runtime-test/sanitizer/压力并发仍未由静态门执行。

## 2026-09-13 Spec185 B5 normal compile/runtime boundary v7-v4

同一 build tree 以 `-j4` 重建 `di-native-assembly-worker` 返回 `rc=0`，新的 worker 身份记录在 `.codex-tmp/spec185-b5/normal-build-v7.worker.sha256`；该修复使两个 assembler oracle 通过。随后完整 selector v4 返回 `rc=201`，7/8 用例通过；唯一失败为 authenticated fixture 在 response publication 出口早于 Provider counter 更新，后续异步拒绝循环的最后一次出现空 candidate 并 abort。原始日志见 `.codex-tmp/spec185-b5/normal-run-v4/output.log`，不能计 T009/T010 完成。

## 2026-09-13 Spec185 B5 T010 repair review pass v29

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v29` 返回 `T010_STATIC_PASS`，无 P0/P1/P2/P3；fixture 使用生产 `canonicalOnnxSourceIdentity` 派生真实 graph/initializer digest，v28 completion 修复保持，五 lane 无静态回退。compile-link/runtime-test/sanitizer/压力并发仍未执行。

## 2026-09-13 Spec185 B5 T010 static boundary v30

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v30` 返回 `STATIC_FAIL`；fixture 异步 response/failure 状态未统一同步，受控失败循环在 terminal callback 前返回并重置可变请求状态，存在跨线程数据竞争和旧回调污染风险。未构建/运行，T010 保持 `PARTIAL`，详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#t010-static-boundary-v30)。

## 2026-09-13 Spec185 B5 T010 repair review pass v31

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v31` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；每请求 probe 的同步、不可变请求捕获和 terminal 等待已闭合。compile-link/runtime-test/sanitizer 尚未执行，T010 保持 `PARTIAL`，详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#t010-repair-review-pass-v31)。

## 2026-09-13 Spec185 B5 normal runtime boundary v6

候选 `spec185-provider-assembly` SHA256 `a4dcee113eb57a4bd1bdd5101377eb9048c6fa1f13f4cfc2174e29fd879de1b8`，worker SHA256 `a33266011e5f95351d3dc65d3053e78092c7db3a5f1ae707cfc202d5cac62e7e` 的完整 selector 返回 `rc=201`，7/8 用例通过；唯一失败是 authenticated fixture 在异步回调中的 Boost.Test 断言 SIGSEGV（地址 `0x80`，最后 checkpoint 为 `di-prepared-provider.t.cpp:668`）。该候选早于 v31 修复，不计产品通过；原始记录见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-normal-runtime-boundary-v6)。

## 2026-09-13 Spec185 B5 normal compile pass v10

v31 静态复审后既有 `build-spec185-b0c-normal` 以系统优先 PATH、`-j4` 仅构建 `spec185-provider-assembly` 成功，耗时约 `26.097s`，`rc=0`，候选 SHA256 `4261653e2829172da85a5b1dd1983b8753327ac7a2e9d745d6764e8f0589a878`。compile-link PASS，runtime-test/sanitizer 尚未执行，详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-normal-compile-pass-v10)。

## 2026-09-13 Spec185 B5 normal runtime boundary v7

v31 候选完整 selector 返回 `rc=201`，13 项断言失败；User PubSub fixture 未将 Response publication 转给 `handleDecryptedResponseByName`，正向请求 timeout 且 assembly/runner counters 为 0，负向循环未形成终态并在 `facade.drain` 失败后 SIGABRT。原始记录见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-normal-runtime-boundary-v7)。T009/T010 保持 `PARTIAL`，修复后必须静态复审、重建和重跑。

## 2026-09-13 Spec185 B5 T010 static boundary v32

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v32` 返回 `STATIC_FAIL`；fixture 将生产 SVS `HybridMessageEnvelope` 原始 wire 直接传给只接受已解密 `ResponseMessage` TLV 的 `handleDecryptedResponseByName`，绕过生产 `OnResponse` 的 decryptHybridMessage 路径，response oracle 无效。未构建/运行，T010 保持 `PARTIAL`，详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#t010-static-boundary-v32)。

## 2026-09-13 Spec185 B5 T010 repair review pass v33

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v33` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；fixture 已移除原始 HybridMessageEnvelope 到 decrypted API 的直传，恢复生产 `ServiceUser::OnResponse`/`OnRequestAck` 解密路径并补齐同源 ACK/RESPONSE/SELECTION 测试密钥。compile-link/runtime-test/sanitizer 尚未执行，T010 保持 `PARTIAL`，详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#t010-repair-review-pass-v33)。

## 2026-09-13 Spec185 B5 normal compile pass v11

v33 静态复审后既有 `build-spec185-b0c-normal` 以系统优先 PATH、`-j4` 仅构建 `spec185-provider-assembly` 成功，耗时约 `19.738s`，`rc=0`，候选 SHA256 `c280d8104d060959ffbf98c72653c3eab246fe47b326ed1b7d6b35f25bbad0fb`。compile-link PASS，runtime-test/sanitizer 尚未执行，详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-normal-compile-pass-v11)。

## 2026-09-13 Spec185 B5 normal runtime boundary v8

v33 候选完整 selector 返回 `rc=201`，13 项断言失败；同源 ACK/RESPONSE/SELECTION key setup 后正向仍无 response/assembly/runner counters，负向循环未见 Selection，末尾 `facade.drain` 失败并 SIGABRT。原始记录见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-normal-runtime-boundary-v8)。T009/T010 保持 `PARTIAL`，先补同步 probe failure 诊断再修复。

## 2026-09-14 Spec185 B5 T010 repair review pass v34

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v34` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；仅新增 mutex 保护下的同步 failure 文本诊断，未改变生产解密或 key setup。compile-link/runtime-test 尚未执行，T010 保持 `PARTIAL`，详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#t010-repair-review-pass-v34)。

## 2026-09-13 Spec185 B5 T010 repair review pass v35

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v35` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；fixture 保留服务级 Provider permission 并增加与生产 admission 构造规则一致的 `/Inference/Spec185ProviderOracle/ROLE/Backbone` role permission，未放宽生产校验。compile-link/runtime-test/sanitizer 尚未执行，T010 保持 `PARTIAL`，详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#t010-repair-review-pass-v35)。

## 2026-09-13 Spec185 B5 normal compile pass v13

v35 静态复审后既有 `build-spec185-b0c-normal` 以系统优先 PATH、`-j4` 仅构建 `spec185-provider-assembly` 成功，耗时约 `20.028s`，`rc=0`，候选 SHA256 `b4af4a1c50d18a5f5ba9f008416d3501b53673293afafc8266e0c226f2219d2f`。compile-link PASS；详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-normal-compile-pass-v13)。

## 2026-09-13 Spec185 B5 normal runtime boundary v10

v13 候选完整 `Spec185ProviderAssembly` selector 返回 `rc=201`，7/8 用例完成；新增 role grant 在 bootstrap 后替换 Provider permission 表并触发刷新副作用，正向 ACK/Selection/Response/assembly/runner 均未形成，负向循环同样未见 Selection，末尾 `facade.drain` 失败并 SIGABRT。原始记录见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-normal-runtime-boundary-v10)。应将 role grant 纳入 bootstrap permission wave，T009/T010 保持 `PARTIAL`。

## 2026-09-13 Spec185 B5 T010 repair review pass v36

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v36` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；`providerRoles` 默认空，Spec185 role grant 在初始 bootstrap ProviderPermission wave 安装，移除 post-bootstrap 权限替换及其刷新副作用。compile-link/runtime-test/sanitizer 尚未执行，T010 保持 `PARTIAL`，详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#t010-repair-review-pass-v36)。

## 2026-09-13 Spec185 B5 normal compile pass v14

v36 静态复审后既有 `build-spec185-b0c-normal` 以系统优先 PATH、`-j4` 仅构建 `spec185-provider-assembly` 成功，耗时约 `21.350s`，`rc=0`，候选 SHA256 `7f22f35a282f8e624f6a7630e992807a5333e812d636928c102110ec8ffd5f09`。compile-link PASS；详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-normal-compile-pass-v14)。

## 2026-09-13 Spec185 B5 normal runtime boundary v11

v14 候选完整 `Spec185ProviderAssembly` selector 返回 `rc=201`，7/8 用例通过；bootstrap role grant 已安装但正向和三个负向请求均未形成 Selection，正向 assembly/runner/Response 未发生，末尾 `facade.drain` 失败并 SIGABRT。原始记录见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-normal-runtime-boundary-v11)。T009/T010 保持 `PARTIAL`，继续定位 ACK/Selection ingress。

## 2026-09-13 Spec185 B5 diagnostic runtime boundary v12

TRACE 诊断运行 v14 候选确认正向 REQUEST、ACK、Selection、Provider execution 已进入生产路径；首个失败是 `validateNativePreparedRunnerSpec` 返回 `DI_PROVIDER_ASSEMBLY_PATH_UNSAFE`，fixture runner spec 使用 `oracle.onnx` 而契约要求绝对路径文件名 `model.onnx`，assembly/runner/Response 未发生。原始记录见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-diagnostic-runtime-boundary-v12)。该诊断不计 runtime PASS，已修正路径并补 identity metadata，待静态复审。

## 2026-09-13 Spec185 B5 T010 static boundary v37

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v37` 返回 `STATIC_FAIL`；preparationFactory 删除了 fixture runner factory 通过 `metadata.at()` 读取的 `provider/boot/plan/artifact` 字段，首个 `create()` 会抛 `std::out_of_range`，正向 assembly/runner/Response 无法发生。未构建/运行，T010 保持 `PARTIAL`，详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#t010-static-boundary-v37)。

## 2026-09-13 Spec185 B5 T010 repair review pass v38

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v38` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；fixture 恢复 runner factory 所需 `provider/boot/plan/artifact` metadata，同时保留 `model.onnx` 路径和完整 assembly identity。compile-link/runtime-test/sanitizer 尚未执行，T010 保持 `PARTIAL`，详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#t010-repair-review-pass-v38)。

## 2026-09-13 Spec185 B5 normal compile pass v15

v38 静态复审后既有 `build-spec185-b0c-normal` 以系统优先 PATH、`-j4` 仅构建 `spec185-provider-assembly` 成功，耗时 `38.016s`，`rc=0`，候选 SHA256 `7f94e4505797f9f04fc704f874cc0da1bc1ff59cbbc84f9bb7f6b451651fd0ee`。compile-link PASS；详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-normal-compile-pass-v15)。

## 2026-09-13 Spec185 B5 normal runtime boundary v12

v15 候选完整 selector 返回 `rc=201`，7/8 用例通过；正向 ACK/Selection/Response 已形成但 assembly counter 为 0，三个身份替换负向用例未见 Selection，末尾 `facade.drain` 失败并 SIGABRT。原始记录见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-normal-runtime-boundary-v12)。T009/T010 保持 `PARTIAL`，待 TRACE 定位 assembly 与负向请求边界。
## B5 diagnostic runtime boundary v13

在不改变候选源码的条件下，以 `NDN_LOG=ndn_service_framework.ServiceProvider=TRACE:ndn_service_framework.ServiceUser=TRACE:ndn_svs.SVSPubSub=TRACE` 运行 v15 候选，候选 SHA256 为 `7f94e4505797f9f04fc704f874cc0da1bc1ff59cbbc84f9bb7f6b451651fd0ee`，worker SHA256 为 `a33266011e5f95351d3dc65d3053e78092c7db3a5f1ae707cfc202d5cac62e7e`，返回 `rc=201`。TRACE 确认正向 REQUEST、ACK、Selection、Provider execution、Response publication、User 解密和 callback 均已闭合；正向唯一失败是测试 `ProviderCounters.assemblies` 为 `0`，因为 test preparation seam 覆盖了生产 preparation wrapper，未进入生产 metrics 计数点。三个负向请求使用 `/spec185-provider-reject-/N`，被解析为 service `/Inference/Spec185ProviderOracle/spec185-provider-reject-`、requestId `/N`，首个失败在 service-level permission admission，未进入 Selection。原始日志、退出码和候选身份见 `.codex-tmp/spec185-b5/diagnostic-run-v13/`。该诊断不计 runtime PASS；下一步修正 test seam 计数并使用单一 NDN component 的负向 ID，需重新静态审查、构建和运行，T009/T010 保持 `PARTIAL`。

## B5 T010 repair review pass v39

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v39` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；test preparation seam 仅在成功返回 spec 后递增 `ProviderMetrics::assemblies`，三个负向 request ID 使用单一 NDN component，v38 seam metadata、assembly identity、`model.onnx` 路径及 validator 约束保持。五 lane 全部 covered；未执行 compile-link/runtime-test/sanitizer，T009/T010 保持 `PARTIAL`，需运行新的 C++ 候选验证动态计数与拒绝终态。
## B5 normal compile pass v16

v39 静态复审后，在既有 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、系统优先 PATH 下以 `-j4` 仅构建 `spec185-provider-assembly` 成功，耗时 `21.664s`，返回 `rc=0`。候选 SHA256 为 `a111e193ef01c07793089e86c284e60611f2f236a01d5c96c6b2d699be20ae6e`；完整日志、退出码、资源记录和候选身份见 `.codex-tmp/spec185-b5/normal-build-v16/output.log`、`.rc`、`.vmstat.log`、`.binary.sha256`。compile-link PASS；runtime-test/sanitizer 待执行，T009/T010 保持 `PARTIAL`。
## B5 normal runtime boundary v13

v16 候选完整 `Spec185ProviderAssembly` selector 返回 `rc=201`，`105/107` assertions 通过。正向 REQUEST、ACK、Selection、Provider assembly seam、runner、Response publication、User 解密及三类 provider/epoch/grant substitution rejection 均通过；唯一失败为 `facade.drain(2000ms)` 返回 false，随后测试 abort。候选 SHA256 为 `a111e193ef01c07793089e86c284e60611f2f236a01d5c96c6b2d699be20ae6e`，worker SHA256 为 `a33266011e5f95351d3dc65d3053e78092c7db3a5f1ae707cfc202d5cac62e7e`；原始输出、退出码和身份见 `.codex-tmp/spec185-b5/normal-run-v13/`。本结果不计 runtime PASS；待修正 fixture 的 stop/drain 调用出口后重新静态审查、构建和运行，T009/T010 保持 `PARTIAL`。
## B5 T010 repair review pass v40

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v40` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；fixture 明确 `registration.close()` → `facade.stop()` → `facade.drain(2000ms)`，先升 Provider stopped fence，再等待 borrowed Face 的 IO barrier/join，重复 close 保持幂等且 drain 失败仍传播。五 lane 全部 covered；未执行 compile-link/runtime-test/sanitizer，T009/T010 保持 `PARTIAL`。
## B5 normal compile pass v17

v40 静态复审后，在既有 `build-spec185-b0c-normal`、`.lock-spec185-b0c-normal`、系统优先 PATH 下以 `-j4` 仅构建 `spec185-provider-assembly` 成功，耗时 `22.994s`，返回 `rc=0`。候选 SHA256 为 `8683a0902628355bb261b6923bd091a9146a80c8cf31d58c29f50b11e779bde8`；完整日志、退出码、资源记录和候选身份见 `.codex-tmp/spec185-b5/normal-build-v17/output.log`、`.rc`、`.vmstat.log`、`.binary.sha256`。compile-link PASS；runtime-test/sanitizer 待执行，T009/T010 保持 `PARTIAL`。
## B5 normal runtime pass v14 (first repeat)

v17 候选完整 `Spec185ProviderAssembly` selector 首次修复后返回 `rc=0`，8/8 test cases、106/106 assertions 通过；正向 authenticated assembly/runner/Response、三类 provider/epoch/grant rejection、Provider-only lifecycle、canonical assembler/source fetch、cold/hit cache、protected binding substitution 和 stop→drain 均通过。候选 SHA256 为 `8683a0902628355bb261b6923bd091a9146a80c8cf31d58c29f50b11e779bde8`，worker SHA256 为 `a33266011e5f95351d3dc65d3053e78092c7db3a5f1ae707cfc202d5cac62e7e`；原始输出、退出码和身份见 `.codex-tmp/spec185-b5/normal-run-v14/`。按 C-04 每 case 两次，尚需独立 sanitizer，T009/T010 保持 `PARTIAL`。

## 2026-09-14 Spec185 B5 normal runtime pass v15 (second repeat)

使用与 v14 完全相同的候选 `8683a0902628355bb261b6923bd091a9146a80c8cf31d58c29f50b11e779bde8` 和 worker `a33266011e5f95351d3dc65d3053e78092c7db3a5f1ae707cfc202d5cac62e7e`，第二次完整 `Spec185ProviderAssembly` selector 返回 `rc=0`，8/8 test cases、106/106 assertions 通过；正向 assembly/runner/Response、三类身份拒绝、canonical source、cache cold/hit、protected binding substitution 与 stop→drain 均再次通过。原始输出和身份见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-normal-runtime-pass-v15-second-repeat) 及 `.codex-tmp/spec185-b5/normal-run-v15/`。C-04 normal repeat 已闭合；独立 sanitizer 与 B5 批次组合审查仍待执行，T009/T010 保持 `PARTIAL`。

## 2026-09-14 Spec185 B5 normal compile pass v18 and runtime repeats v16/v17

v44 静态复审及 B5 composition v3 后，既有 normal build tree 以系统优先 PATH、`-j4` 仅构建 `spec185-provider-assembly` 成功，`rc=0`，候选 SHA256 `dbdd7117fb2756049e35768c5535e797d7edb60563a69ede5712729bfe80a53b`。该候选的完整 `Spec185ProviderAssembly` selector 连续两次返回 `rc=0`，每次输出 `Running 8 test cases` 与 `*** No errors detected`；worker SHA256 `a33266011e5f95351d3dc65d3053e78092c7db3a5f1ae707cfc202d5cac62e7e`。原始记录见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-normal-compile-pass-v18)、[runtime v16](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-normal-runtime-pass-v16-first-repeat-after-v44)、[runtime v17](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-normal-runtime-pass-v17-second-repeat-after-v44)。normal compile-link 与 C-04 normal repeat 已通过；独立 sanitizer 尚未重建，T009/T010 保持 `PARTIAL`。

## 2026-09-14 Spec185 B5 sanitizer runtime boundary r1

独立 `build-spec185-b3-asan-ubsan-fast` 配置以 `-j4` 重建 `spec185-provider-assembly` 成功，候选 SHA256 `fef9915218f1e355eb8494ca44d4472990ffa2241b443aebdf5edda4158b70d3`；严格 ASan/UBSan 完整 selector 业务断言到 `*** No errors detected`，但退出阶段 LSan 报告 27 个间接泄漏、4518 bytes，首个栈位于 Provider-only `Runtime::open` 构造触发的 NAC-ABE `SegmentFetcher`/Face 资源，`rc=134`。单独 Provider-only selector 重现同一边界；原始记录见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-sanitizer-runtime-boundary-r1) 及 `.codex-tmp/spec185-b5/asan-run-r1-provider-only/`。该结果不计 sanitizer PASS，已安排 Provider stop 后 Face-bound owner 释放修复，待 v41 静态复审、重建和重复验证，T009/T010 保持 `PARTIAL`。

## 2026-09-14 Spec185 B5 T010 static boundary v41

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v41` 返回 `STATIC_FAIL`；新增 `releaseStoppedResources()` 与 `serve()`、`stop()`、`drain()` 的 shared_ptr 成员访问未统一同步，Provider copy 与 reaper release 可能形成数据竞争或空对象/UAF，属于控制性 P1。未构建/未运行，详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#t010-static-boundary-v41)；已修复并派发 v42 复审，T009/T010 保持 `PARTIAL`。

## 2026-09-14 Spec185 B5 T010 repair review pending v42

v41 P1 修复后冻结 `.codex-tmp/spec185-b5-t010-static-v42`，changes SHA256 `a34d571969369c93d1034519149132e657baa6a67b887139aa5ec2df91ec542e`，paths SHA256 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`；已重新派发官方只读静态复审，未构建/运行，T009/T010 保持 `PARTIAL`。

## 2026-09-14 Spec185 B5 T010 static boundary v42

官方 `review-agent` 发现 `requestStopIo()` 在 `ioMutex` 内调用 `releaseStoppedResources()`，与并发 `serve()` 的 `serveMutex`→`ioMutex` 路径构成锁顺序反转，属于控制性 P1；未构建/未运行，详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#t010-static-boundary-v42)。已将无 IO 线程分支释放移到临界区之后，待 v43 复审，T009/T010 保持 `PARTIAL`。

## 2026-09-14 Spec185 B5 T010 repair review pending v43

v42 P1 修复后冻结 `.codex-tmp/spec185-b5-t010-static-v43`，changes SHA256 `298a423a68526720c76507fa6af111e804f76f8742901d83d24904f32121d375`，paths SHA256 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`；已重新派发官方只读静态复审，未构建/运行，T009/T010 保持 `PARTIAL`。

## 2026-09-14 Spec185 B5 T010 static boundary v43

官方 `review-agent` 发现异步 Face 回调引用捕获栈对象/`this`，stop 唤醒 waiter 后存在悬空引用 P1；self-thread `stopIo` reaper 未调用 owner release，存在资源长期保留 P2。详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#t010-static-boundary-v43)；已修复并待 v44 复审，T009/T010 保持 `PARTIAL`。

## 2026-09-14 Spec185 B5 T010 repair review pending v44

v43 P1/P2 修复后冻结 `.codex-tmp/spec185-b5-t010-static-v44`，changes SHA256 `f408627be979ebab47410e9684273d3d15d1e96e5cdd0f4fb73aaf3b1f1d4b94`，paths SHA256 `c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`；已重新派发官方只读静态复审，未构建/运行，T009/T010 保持 `PARTIAL`。

## 2026-09-14 Spec185 B5 T010 repair review pass v44

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v44` 返回 `STATIC_PASS`，无 P0/P1/P2/P3；`ServeInvocation`、self-thread reaper、无线程分支、锁序和幂等 owner release 均通过静态检查。快照身份见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#t010-repair-review-pass-v44)。待修复候选 normal/sanitizer compile-link 与 runtime，T009/T010 保持 `PARTIAL`。

## 2026-09-14 Spec185 B5 composition review pass v3

官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-composition-v3` 返回 `B5_COMPOSITION_PASS`，无控制性缺陷；五 lane、ServeInvocation owning payload、owner release 锁序、fixture/build closure 及证据边界均覆盖。快照身份见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-composition-review-pass-v3)。Batch growth=`STOP_GROWTH`、static closure=`CLOSED_FOR_VALIDATION`；待修复候选 normal/sanitizer compile-link 与 runtime，T009/T010 保持 `PARTIAL`。

## 2026-09-13 Spec185 B5 normal compile boundary v19

组合 v4 静态门后，以系统优先 PATH、`CC=/usr/bin/gcc`、`CXX=/usr/bin/g++` 和 `-j4` 仅构建 `spec185-provider-assembly`，返回 `rc=1`。首个边界为 `ProviderReaperOwner::~ProviderReaperOwner()` 将 `*target`（`std::thread`）传给要求 `std::unique_ptr<std::thread>&` 的 `finish`；生产/fixture 尚未进入链接。原始输出见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-normal-compile-boundary-v19) 及 `.codex-tmp/spec185-b5/normal-build-v19/`。已修正为 `finish(target)`，需受影响静态复审后重建，T009/T010 保持 `PARTIAL`。

## 2026-09-14 Spec185 B5 T010 static repair boundary v60-v63

官方 `review-agent` 在 v60、v62 分别发现 borrowed Face worker stopped-context 终态未发布及 stop marker/context race 导致无界 join 两个 P1；v63 修复后静态 `PASS`，五 lane 闭合。v63 快照 `.codex-tmp/spec185-b5-t010-static-v63` 的 changes SHA256=`f841d07333edb6cd034d2d0d38d3fa6167571edbdebd7a961fcb0ae99b5513ba`，paths SHA256=`c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`，详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#t010-repair-review-pass-v63)。

## 2026-09-14 Spec185 B5 normal compile/runtime boundary v21/v19-v20

v63 静态门后，既有 build tree 以系统优先 PATH、`-j4` 仅构建 `spec185-provider-assembly`，`rc=0`，候选 SHA256=`67ebec894a8320fec491e2891b6f23dab6209910b7546308cf61c01d14520f9e`。同一候选完整 selector 两次均在 authenticated Provider 用例发生内存破坏：v19 `rc=134`（`double free or corruption`/`malloc invalid size`），v20 `rc=201`（invalid permissions/memory access）。gdb v22 首个信号位于主线程 `ndn::Buffer` shared_ptr 引用计数路径并经过 `ndn::svs::SVSPubSub` 回调，Provider borrowed Face worker 同时泵共享 `io_context`；尚未证明根因属于生产或 fixture。原始记录见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-normal-runtime-boundaries-v19-v20) 和 `.codex-tmp/spec185-b5/diagnose-v22/gdb-auth.log`。这是动态内存/并发失败边界，不计 T009/T010 完成；下一步必须先以 sanitizer 或最小化 C++ 诊断定位首个边界。

2026-09-14 03:20 -05:00 B5 sanitizer diagnostic boundary v23：ASan/UBSan 候选重建成功后，authenticated selector 首个报告为 `heap-use-after-free`：主线程在 `ndn::svs::Fetcher::onData` 复制 `Fetcher::QueuedInterest` 回调时读取已由 Provider worker 线程释放的 `std::function`。调用链经过 `DummyClientFace::receive`，Provider worker 由 `Provider::startIo()` 在 borrowed Face 共享 `io_context` 上并发 `run_one_for()`；原始报告及双线程分配/释放栈见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-sanitizer-diagnostic-boundary-v23) 与 `.codex-tmp/spec185-b5/diagnose-v23/gdb-asan-auth.log`。这是 fixture/borrowed Face 事件循环接线的动态内存失败边界，尚未修复或复测，T009/T010 保持 `PARTIAL`。

2026-09-14 03:27 -05:00 B5 T010 static boundary v64：官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v64` 返回 `STATIC_FAIL`，changes SHA256=`c27660baa2dff9fa6ead55ce8df2e4187f0d5a11a8d4abc68a726376b15ed9ba`，paths SHA256=`c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。P1 为 bootstrap 在 worker 启动前跳过 Provider Face 泵送，以及 deferred bridge 对 active faults、pending packets、bridge stats 的跨线程未同步访问。未构建/运行；详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#t010-static-boundary-v64)，T009/T010 保持 `PARTIAL`，已修复范围待 v65 复审。

2026-09-14 04:10 -05:00 B5 T010 static boundary v65：v64 两个 P1 已闭合；官方 `review-agent` 新发现 P2，`forwardInterest()` 的 duplicate/reorder 组合少记实际投递与重复计数，stream-interest drop 限额在解锁读取后递增存在并发超限。详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#t010-static-boundary-v65)；未构建/运行，T009/T010 保持 `PARTIAL`，已修复范围待 v66 复审。

2026-09-14 04:30 -05:00 B5 T010 static boundary v66：v65 的 bootstrap、bridge 状态同步和计数修复已通过；官方 `review-agent` 新发现 P2，`reorderPackets && duplicatePackets` 的 Interest/Data 线序从旧契约 `current,current,pending,pending` 回退为 `current,pending,current,pending`。详见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#t010-static-boundary-v66)；未构建/运行，T009/T010 保持 `PARTIAL`，已修复范围待 v67 复审。

2026-09-14 04:45 -05:00 B5 T010 repair review pass v67：官方 `review-agent` 对冻结快照 `.codex-tmp/spec185-b5-t010-static-v67` 返回 `STATIC_PASS`，changes SHA256=`d344dd4984836aefdf062be7e44c1b5fea8b10f31994bf41b8b4b6e64d029c14`，paths SHA256=`c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。Interest/Data reorder+duplicate 线序与计数恢复旧契约，stream-interest 限额、bootstrap worker 切换和 deferred bridge 状态锁通过五 lane 静态门；未构建/运行，T009/T010 保持 `PARTIAL`，进入批末 dynamic validation。

2026-09-14 05:20 -05:00 B5 dynamic validation pass：v67 修复候选 normal compile v22 `rc=0`；authenticated focused v21 及完整 selector v22/v23 各 `rc=0`，8/8 cases、无 Boost.Test 错误。独立 ASan/UBSan+LSan compile v3 `rc=0`，完整 selector v2/v3 各 `rc=0`、无 sanitizer 报告。修复前 v19/v20 内存破坏和 v23 UAF 仍作为历史失败边界保留；B5 composition review 尚待执行，T009/T010 仍为 `PARTIAL`，动态结果不单独构成批次闭合。

2026-09-13 22:32 -05:00 B5 composition closure pass：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-b5-composition-v5` 返回 `B5_COMPOSITION_PASS`，无 P0/P1/P2/P3；快照 base=`775d0687d97eeb6c9f053d8e78c8c01b31864f91`、changes SHA256=`d344dd4984836aefdf062be7e44c1b5fea8b10f31994bf41b8b4b6e64d029c14`、paths SHA256=`c87a85173ff491d912839721a46e63c8eb703b6e7f7f14a18d63891182e08bac`。五 lane 与 B5 生产调用链闭合；normal compile/runtime 与独立 sanitizer compile/runtime 均已通过，T009/T010 完整验收完成并在 tasks.md 标为 `[x]`。证据见 [b5-provider](../specs/185-prepared-model-runtime/evidence/b5-provider.md#b5-composition-review-pass-v5-and-closure)。

2026-09-14 01:42 -05:00 B7 T013 compile boundary v1：v10 static/composition gate 已通过；普通系统优先 `-j4` 构建在 `tests/integration-tests/di-prepared-process.t.cpp` 翻译阶段首错 `requireAbsentMarker was not declared`，返回 `rc=1`，未链接或运行。原始日志 `.codex-tmp/spec185-b7/process-build-v1.log`、资源记录 `.codex-tmp/spec185-b7/process-build-v1.vmstat.log`，批次证据见 [b7-cpp-qualification](../specs/185-prepared-model-runtime/evidence/b7-cpp-qualification.md)。已补回 C++ helper；按静态门规则需重新冻结并复审受影响范围，T013 保持 `PARTIAL`。
2026-09-14 06:52 -05:00 B7 T013 compile-link boundary v2：v11 static/composition gate 已通过；普通系统优先 `-j4` 构建成功链接 `spec185-process`，随后在既有 `integration-tests` 链接阶段首错为 `ndn_service_framework::OperationRuntime` 的 `notifyWaiters`、`drain`、`drainAsync`、`create`、`close` 未定义，返回 `rc=1`，未运行 selector。原始日志 `.codex-tmp/spec185-b7/process-build-v2.log`、资源记录 `.codex-tmp/spec185-b7/process-build-v2.vmstat.log`，批次证据见 [b7-cpp-qualification](../specs/185-prepared-model-runtime/evidence/b7-cpp-qualification.md)。定位为 `tests/wscript` 手工 `framework_sources` 闭包遗漏 `ndn-service-framework/OperationRuntime.cpp`；修复后需重新冻结并静态复审接线，T013 保持 `PARTIAL`。
2026-09-14 01:53 -05:00 B7 T013 repair static boundary v12：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t013-review-v12-20260914` 返回 `STATIC_PASS`，无 P0-P3；changes SHA256=`e7490697a283386df5c49731360e8e91e6adf036f519018a2eefc4cab0f32712`，paths SHA256=`7ebf1bbebff4e3fd77ef7a5bad58ce657c9adc21574388d4c877ada06e13fed2`。确认 `OperationRuntime.cpp` 仅补入 `integration-tests` 手工 framework source closure，五 lane 覆盖；未构建/运行，T013 保持 `PARTIAL`，进入新的 compile-link 尝试。
2026-09-14 01:55 -05:00 B7 T013 compile-link recovery：v12 静态门后，系统优先 `-j4` 构建 v3 成功链接 `spec185-process`、`spec185-prepared-request`、`spec185-provider-assembly` 和 `integration-tests`，耗时 18.80 秒，返回 `rc=0`，无持续 swap。原始日志 `.codex-tmp/spec185-b7/process-build-v3.log`、资源记录 `.codex-tmp/spec185-b7/process-build-v3.vmstat.log`；这是链接边界恢复，不是运行或资格 PASS，T013 保持 `PARTIAL`。
2026-09-14 01:56 -05:00 B7 T013 composition review v12：官方 `review-agent` 对同一不可变快照返回 `B7_COMPOSITION_PASS`，无 P0-P3；C++ process/native oracle、Python lifecycle orchestration、C-04 矩阵、双次 selector、超时、日志边界和 `OperationRuntime.cpp` 闭包均通过组合审查。运行、sanitizer 与残留进程清理尚未验证，T013 保持 `PARTIAL`。
2026-09-14 02:03 -05:00 B7 T013 runtime boundary v1：批末 `spec185-process` 首次运行返回 `rc=201`，三个独立边界均已保留原始记录。stream conversation 的 native requester 在 input 阶段返回 `UNSUPPORTED_CAPABILITY`（catalog 缺失 `conversation_input` 编码声明）；`spec185-prepared-request` 的 `PreparedRequestCompletesThroughProvider` 在 `NDNSF_INTEGRATION_BOOTSTRAP_READY` 后等待结果超时；真实 revoke 编排因 Controller 设为 60 秒而 Python 只等待 30 秒，未观察到 `NDNSF_REVOCATION_APPLIED success=1`。原始目录见 `.codex-tmp/spec185-b7/runtime-v1/`，主日志 `.codex-tmp/spec185-b7/process-runtime-v1.log`；不计 runtime PASS，T013 保持 `PARTIAL`。
2026-09-14 02:08 -05:00 B7 T013 runtime repair static boundary v13：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t013-review-v13-20260914` 返回 `STATIC_PASS`，无 P0-P3；changes SHA256=`ef3688ef12e744a9be5f4abe4eeb50c30e7a1a4778295f8dcdaf442fb1001c2c`，paths SHA256=`6b717d23f180e720eab4a9d61c236614df9f23dd88b04387a8bb504252488e27`。确认 stream conversation encoder、revoke 等待预算与 C++ oracle 接线；复用同一 v3 binary，进入 runtime 重试，未将静态通过计为资格 PASS。
2026-09-14 02:16 -05:00 B7 T013 focused timeout boundary：仅运行 `Spec185PreparedRequest/PreparedRequestCompletesThroughProvider` 的诊断仍在 `NDNSF_INTEGRATION_BOOTSTRAP_READY` 后约 5.8 秒返回 `native request budget expired`（`rc=201`），未进入结果或清理断言。原始记录 `.codex-tmp/spec185-b7/prepared-timeout-focus-v1.log`、`.rc`；未修改源码，不计资格 PASS，T013 保持 `PARTIAL`。
2026-09-14 02:22 -05:00 B7 T013 focused trace boundary：`prepared-timeout-trace-v1.log` 显示 request `...-1` 在约 4.74 秒已发布并接收 response；并发 request `...-2` 的 ACK 解密回调晚于其 2 秒 ACK deadline，触发 `ACK_SKIPPED_AFTER_ACK_WINDOW` 并最终预算过期。诊断指向 fixture 的 5s/2s 请求预算过紧，未推断生产故障；调整测试 fixture 前需静态复审，T013 保持 `PARTIAL`。
2026-09-14 02:31 -05:00 B7 T013 timeout fixture static boundary v14：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t013-review-v14-20260914` 返回 `STATIC_PASS`，无 P0-P3；changes SHA256=`263e8d14cb23debec2f383980cc39ccc1ebb57b7ff0d961dcef1466b2476f530`，paths SHA256=`20201a5c1c58efed82f64557270108bb576b169f7564f82d97f91276772ab7ed`。确认 10s/5s fixture budget、conversation encoder、revoke ordering、C++ oracle 与 Waf closure；未构建/运行，T013 保持 `PARTIAL`。
2026-09-14 02:33 -05:00 B7 T013 compile-link v4：v14 静态门后系统优先 `-j4` 重建同一四目标集合，返回 `rc=0`，耗时 28.79 秒；`spec185-prepared-request` 重新编译链接，其余目标复用同一 build tree，资源记录无持续 swap。运行和 sanitizer 尚未验证，T013 保持 `PARTIAL`。
2026-09-14 02:45 -05:00 B7 T013 focused retry boundary v2：10s/5s fixture budget 后，单独 `PreparedRequestCompletesThroughProvider` 仍在约 10.7 秒返回 `native request budget expired`（`rc=201`，last checkpoint 883）。原始记录 `.codex-tmp/spec185-b7/prepared-timeout-focus-v2.log`、`.rc`；预算调整未闭合该边界，需 trace 诊断后再修改，T013 保持 `PARTIAL`。
2026-09-14 03:00 -05:00 B7 T013 focused trace/diagnostic boundary v2：`.codex-tmp/spec185-b7/prepared-timeout-trace-v2.log`（`rc=201`）显示两个 ACK 均在约 100ms 内匹配、`ackWindowExpired=false`，但没有 `COLLAB_ACK_CLOSED`、selection 或 Provider execution，最终 request budget expired。临时日志确认 ACK timeout 分支的两个 schedule 标志和 5000ms budget 均为真，却没有 `ACK_TIMEOUT_CALLBACK`；`.codex-tmp/spec185-b7/prepared-timeout-strace-diagnostic-output-v1.log` 也未见 5 秒 ACK timerfd。该边界指向生产 scheduler/state-machine 调度问题，未计 runtime PASS；T013 保持 `PARTIAL`，证据见 [b7-cpp-qualification](../specs/185-prepared-model-runtime/evidence/b7-cpp-qualification.md)。
2026-09-14 07:58 -05:00 B7 T013 bounded-pump repair review v15：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t013-review-v15-20260914` 返回 `STATIC_PASS`，无 P0-P3；changes SHA256=`37db1c903cf8299b5a53404816b754bc39934fe0a1c697d19819ad3ab9be3bf4`，paths SHA256=`20201a5c1c58efed82f64557270108bb576b169f7564f82d97f91276772ab7ed`。确认 C++ fixture 在 future 等待前最多继续四轮 `pumpUntil`，覆盖 5 秒 ACK scheduler 边界，不改变生产 timeout/owner 语义；compile-link、runtime、sanitizer 仍待执行，T013 保持 `PARTIAL`，详见 [b7-cpp-qualification](../specs/185-prepared-model-runtime/evidence/b7-cpp-qualification.md)。
2026-09-14 03:04 -05:00 B7 T013 compile-link v5：v15 静态门后，以系统优先 PATH、`-j4` 重建 `spec185-process`、`spec185-prepared-request`、`spec185-provider-assembly` 和 `integration-tests`，`rc=0`，耗时 107.22 秒；因当前工作树时间戳重新编译 `ServiceUser.cpp` 与聚焦 fixture，未见持续 swap。原始日志 `.codex-tmp/spec185-b7/process-build-v5.log`、`.rc`、`.vmstat.log`；仍未计入 runtime 或 sanitizer PASS，T013 保持 `PARTIAL`，详见 [b7-cpp-qualification](../specs/185-prepared-model-runtime/evidence/b7-cpp-qualification.md)。
2026-09-14 03:04 -05:00 B7 T013 focused runtime retry v3：使用 v5 候选仅运行 `Spec185PreparedRequest/PreparedRequestCompletesThroughProvider`，`rc=0`，约 11.09 秒，`*** No errors detected`。四轮有界 `pumpUntil` 让真实 User Face scheduler 跨过 5 秒 ACK 窗口；这是聚焦 C++ 运行证据，完整 process 矩阵、sanitizer、清理和批次组合收口仍待执行，T013 保持 `PARTIAL`，详见 [b7-cpp-qualification](../specs/185-prepared-model-runtime/evidence/b7-cpp-qualification.md)。
2026-09-14 03:27 -05:00 B7 T013 repair static v17：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t013-review-v17-20260914` 返回 `STATIC_PASS`，无 P0-P3；`TENSOR_BUNDLE_TOKEN_IDS` 从认证 tensor bundle 的 Int64 `input_ids` 提取会话 token，与执行 coordinator 的 lineage 契约一致，stream fixture 缩进和声明已修复。changes SHA256=`32065d2bdcb3e0da550d9953ddb532108d9c3da691e30fe848edc69202edfa70`，paths SHA256=`56b65200d3bb201f672f0efd7ac07e381f9098cf7bf44e427b715dd3a2d4d51f`；未构建/运行，T013 保持 `PARTIAL`，进入 fresh compile-link，证据见 [b7-cpp-qualification](../specs/185-prepared-model-runtime/evidence/b7-cpp-qualification.md)。
2026-09-14 03:29 -05:00 B7 T013 compile-link v6：v17 static pass 后，以系统优先 PATH、`-j4` 在既有 `build-spec185-b0c-normal` 重建四个目标，`rc=0`，耗时 28.79 秒；`NativeRequestCatalog.cpp` 的各构建变体均重新编译，未见持续 swap。原始日志 `.codex-tmp/spec185-b7/process-build-v6.log`、`.rc`、`.vmstat.log`；仅 compile-link PASS，未计 runtime/sanitizer，T013 保持 `PARTIAL`，证据见 [b7-cpp-qualification](../specs/185-prepared-model-runtime/evidence/b7-cpp-qualification.md)。
2026-09-14 03:32 -05:00 B7 T013 conversation runtime v3：v6 C++ candidate 仅运行 `ConversationRecoveryAndReplacementRemainTerminal`，`rc=0`，约 116.35 秒，`*** No errors detected`。两次 conversation 正向 checkpoint/second-turn、recovery failure、replacement 与 no-backup rejection 均通过 C++ selector 和 native process logs；该子矩阵已通过，完整 process、revoke、sanitizer、清理和最终 composition 仍待执行，T013 保持 `PARTIAL`，证据见 [b7-cpp-qualification](../specs/185-prepared-model-runtime/evidence/b7-cpp-qualification.md)。
2026-09-14 03:32 -05:00 B7 T013 process runtime v3 boundary：v6 candidate 的 unary/stream、conversation/recovery/replacement 与 negative cache/deadline/revoke 前置矩阵通过；`RealCrossProcessRevokeFailsClosed` 仅因 C++ oracle 依赖未稳定发出的 `NDNSF_DI_PROVIDER_HANDLER_TIMING event=start` 失败，Provider baseline 日志已有真实 `event=PROVIDER_EXECUTE_DONE`。原始 `.codex-tmp/spec185-b7/process-runtime-v3.log`、`.rc` 和 `/tmp/spec185-b7-unary-2204547-13/` 保留；未计完整 runtime PASS，T013 保持 `PARTIAL`，证据见 [b7-cpp-qualification](../specs/185-prepared-model-runtime/evidence/b7-cpp-qualification.md)。
2026-09-14 03:39 -05:00 B7 T013 oracle repair static v18：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t013-review-v18-20260914` 返回 `STATIC_PASS`，无 P0-P3；C++ revoke oracle 改为检查真实 Provider `event=PROVIDER_EXECUTE_DONE`，保留 baseline 有执行、Controller revocation marker、revoked requester fail-closed 和撤销后零执行。changes SHA256=`0549e53000e79aadd8665801cbfde66058b39051e9d0768d22ac046c1ee4f1db`，paths SHA256=`b227804f0d93ccae7b4548db087215b801b1f5b25797fdfeacb47f3ff16a2296`；未构建/运行，T013 保持 `PARTIAL`，进入受影响 selector 重建，证据见 [b7-cpp-qualification](../specs/185-prepared-model-runtime/evidence/b7-cpp-qualification.md)。
2026-09-14 03:42 -05:00 B7 T013 compile-link v7：v18 static pass 后以系统优先 PATH、`-j4` 在既有 `build-spec185-b0c-normal` 仅重建 `spec185-process`，`rc=0`，耗时 9.00 秒；C++ process selector 重新编译链接，其他 v6 目标保持同一候选，未见持续 swap。原始日志 `.codex-tmp/spec185-b7/process-build-v7.log`、`.rc`、`.vmstat.log`；仅 compile-link PASS，等待 revoke/runtime 重跑，T013 保持 `PARTIAL`，证据见 [b7-cpp-qualification](../specs/185-prepared-model-runtime/evidence/b7-cpp-qualification.md)。
2026-09-14 03:47 -05:00 B7 T013 final composition v19：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t013-composition-v19-20260914` 返回 `B7_COMPOSITION_PASS`，无 P0-P3；五 lane 与 C-03/C-04/C-06/C-08 调用链、C++ oracle 和 Python lifecycle 边界闭合。changes SHA256=`e3fd98d94cd22d8637cca8d9c62de4c5ee02c60073d59b0775460f84117fac8b`，paths SHA256=`56b65200d3bb201f672f0efd7ac07e381f9098cf7bf44e427b715dd3a2d4d51f`；组合门只证明静态闭合，动态 qualification 仍 `OPEN_FOR_VALIDATION`，T013 保持 `PARTIAL`，证据见 [b7-cpp-qualification](../specs/185-prepared-model-runtime/evidence/b7-cpp-qualification.md)。
2026-09-14 03:52 -05:00 B7 T013 revoke runtime v4：v7 C++ process selector 仅运行 `RealCrossProcessRevokeFailsClosed`，两次均 `rc=0`，约 133.28 秒，`*** No errors detected`。每次通过 baseline success、Controller revocation、revoked requester `DI_NATIVE_NO_ADMITTED_PROVIDER`、failure terminal，以及 Provider baseline `event=PROVIDER_EXECUTE_DONE` 与撤销后零执行断言；该 revoke 子矩阵已闭合，sanitizer、清理和最终提交仍待执行，T013 保持 `PARTIAL`，证据见 [b7-cpp-qualification](../specs/185-prepared-model-runtime/evidence/b7-cpp-qualification.md)。
2026-09-14 04:24 -05:00 B7 T013 fixture owner repair review v22：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t013-review-v22-20260914` 返回 `STATIC_PASS`，无 P0-P3；结果状态/线程 owner 分离，析构取消/连接边界不抛异常，初始化失败取消 active request，正向/负向/drain 移除 `std::async` future predicate。changes SHA256=`c096cdcf2b763d9407863673bcbc419c821cfd5d71f769f50f27ebbd9551a101`，paths SHA256=`4a8f0f88415fa22f9cf6f5f27c43c4da9888b8e637acc73f2f0b007e2e224d09`；未构建/运行前的静态门记录，T013 保持 `PARTIAL`，证据见 [b7-cpp-qualification](../specs/185-prepared-model-runtime/evidence/b7-cpp-qualification.md)。
2026-09-14 04:27 -05:00 B7 T013 final composition review v23：官方 `review-agent` 对不可变快照 `.codex-tmp/spec185-t013-composition-v23-20260914` 返回 `B7_COMPOSITION_PASS`，无 P0-P3；五 lane、C-03/C-04/C-06/C-08 调用链、C++ fixture/oracle、构建闭包和证据边界通过。changes SHA256=`f113752f96814e36081dd021ffa623f24bce6b82c516ed08b5f17ef741c8a690`，paths SHA256=`56b65200d3bb201f672f0efd7ac07e381f9098cf7bf44e427b715dd3a2d4d51f`；动态 qualification 仍 `OPEN_FOR_VALIDATION`，T013 保持 `PARTIAL`，证据见 [b7-cpp-qualification](../specs/185-prepared-model-runtime/evidence/b7-cpp-qualification.md)。
2026-09-14 04:31 -05:00 B7 T013 compile-link v8：v22/v23 静态门后，以系统优先 PATH、`CXX=/usr/bin/g++`、`-j4` 在既有 `build-spec185-b0c-normal` 仅重建受影响 `spec185-prepared-request`，返回 `rc=0`，耗时 24.92 秒；`.codex-tmp/spec185-b7/process-build-v8.log`、`.rc`、`.vmstat.log` 已保留，无持续 swap。仅 compile-link PASS，T013 保持 `PARTIAL`。
2026-09-14 04:32 -05:00 B7 T013 focused normal runtime v4：v8 C++ candidate 的 `PreparedRequestCompletesThroughProvider` 返回 `rc=0`、约 17.87 秒，`*** No errors detected`，覆盖正向 Provider、错误 digest、撤销 fail-closed 与 drain；`PreparedConversationCommitsTwoNativeTurns` 返回 `rc=0`、约 15.52 秒，覆盖两轮 native turn、checkpoint/recovery、replacement 与 drain。原始日志 `.codex-tmp/spec185-b7/prepared-request-runtime-v4.log`、`.rc`、`prepared-conversation-runtime-v4.log`、`.rc` 已保留；sanitizer、清理及最终 T013 资格仍待执行，T013 保持 `PARTIAL`。
2026-09-15 01:50 -05:00 B7 T013 served-provider compile boundary：`prepared-served-build-v1.rc=1` 在新 C++ integration fixture 翻译阶段发现 `nativeProtectedFencingToken` 缺失 `NativeProtectedProvider.hpp` include；未链接/运行，已补 include 后由 v2-v5 构建通过。原始日志 `.codex-tmp/spec185-b7/prepared-served-build-v1.log`，T013 保持 `PARTIAL`。
2026-09-15 01:50 -05:00 B7 T013 served-provider runtime boundary：`prepared-served-focused-v1.rc=201` 在 bootstrap 后因 borrowed `DummyClientFace` 与 deferred bridge delivery 的跨线程调度发生 SIGSEGV；这是测试 fixture 的生命周期/泵送缺陷，原始日志 `.codex-tmp/spec185-b7/prepared-served-focused-v1.log` 已保留，加入 `deferBridgeDelivery` 后需重新静态门和运行。
2026-09-15 01:50 -05:00 B7 T013 served-provider grant boundary：`prepared-served-focused-v2.rc=201` 已到 ACK/grant，但授权器拒绝 `published source is not authorized`；fixture publication source 缺少与 authorized manifest 相同的 model/content/canonical/initializer/artifact-profile digests，已补齐，原始日志 `.codex-tmp/spec185-b7/prepared-served-focused-v2.log` 保留。
2026-09-15 01:50 -05:00 B7 T013 served-provider backend boundary：`prepared-served-focused-v3.rc=201` 已完成 assignment、ACK 与 grant verification，但 runner factory 未注册 offer 选择的 `onnxruntime-cpu` backend；已按 production backend identity 修正 fixture，原始日志 `.codex-tmp/spec185-b7/prepared-served-focused-v3.log` 保留。
2026-09-15 01:50 -05:00 B7 T013 matrix transient boundary：normal v35 与 sanitizer v2 的一次隔离 selector 均在 `RuntimeDrainAsyncIncludesNativeClientWork` 的 60 秒 notification pump 期限失败；该 selector 隔离复跑两次及之后完整 normal/sanitizer matrix 均 `rc=0`，未发现 sanitizer 报告。原始日志 `.codex-tmp/spec185-b7/process-runtime-normal-v35-served-provider.log`、`process-runtime-asan-v2.log` 保留；不将单次超时计为产品 PASS。
2026-09-15 01:50 -05:00 B7 T013 served-provider qualification（历史阶段）：served-provider focused normal/ASan、完整 normal `spec185-process`（5 cases）及 sanitizer C++ matrix（38 isolated selectors）均 `rc=0` 且 `*** No errors detected`；当时 T013 仍为 `PARTIAL`，因为最终 source/ELF/no-Python convergence 尚未刷新。该阶段已由 2026-09-15 final convergence 条目 supersede，生产 canonical assembler/remote authority 仍保留其既定边界。

2026-09-15 06:10 -05:00 B7 T013 final convergence：完整 normal 与 ASan/UBSan（`detect_leaks=0`）候选闭包、五个 process cases（行为 cases 双次、配置 probe 单次）、安装 C++ caller 及 24-artifact ELF/no-Python receipt 均通过，T013 现为 `PASS`。Leak-enabled ASan 仅在外部 `/usr/local/lib/libopenabe.so` 策略树分配上以 `rc=201` 结束，已单列为外部依赖限制；见 [B7 final candidate convergence](../specs/185-prepared-model-runtime/evidence/b7-cpp-qualification.md#b7-final-candidate-convergence-20260915)。
2026-09-15 13:35 -05:00 Local-first SIF APP build gate：使用 Apptainer 1.5.3 和记录中的 `Experiments/TigerCluster/images/spec180-runtime-r119.sif` 执行 pair materialization 输入门禁，首个边界为 `APP_BASE_SIF_PATH_SYMLINK`（该路径是指向不存在 `.local-tmp/spec180-candidate-r119/spec180-runtime.sif` 的 broken symlink），返回 `rc=4`；未进入 Apptainer、builder、APP 提取或运行。原始记录见 `.codex-tmp/local-first-sif-attempt-20260915-r1/`，没有上传或伪造资格结果。取得有效 regular base SIF 后需重新核对摘要，再继续本地 candidate build、APP materialization、C++ runtime 和仅在本地通过后进行 Tiger 交付。
2026-09-15 13:42 -05:00 Local-first base lookup：为取得记录中的 base SIF，仅执行只读 SSH 路径探测；本机无法解析 `tigercluster`（`rc=255`，`Could not resolve hostname tigercluster`），原始命令和输出见 `.codex-tmp/local-first-base-probe-20260915-r1/`。未读取或修改远端文件，未上传；仍需先取得可验证的 regular base SIF，再重跑本地构建门禁。
2026-09-15 13:50 -05:00 Local-first SIF APP closure static review：r44/r48/r49 复审分别发现真实 ELF `DT_NEEDED` 过滤、base allowlist 来源、APP-first `ldd` 环境和 native build 候选路径的测试 oracle 缺陷；均已修正。最终 r50 不可变快照经官方 `review-agent` 返回 `STATIC_PASS`，无 P0-P3，13 个文件哈希匹配；`80 passed, 1 skipped`。动态首边界仍是记录的 base SIF broken symlink（`APP_BASE_SIF_PATH_SYMLINK`, `rc=4`），未进入容器构建/运行，未上传 Tiger。证据见 [SIF APP static check](../Experiments/TigerCluster/docs/sif-app-static-check-20260915.md#r50-native-candidate-path-repair) 和 `.codex-tmp/spec185-sif-app-closure-review-r50/`。
2026-09-15 14:05 -05:00 Local-first SIF APP final static closure：r51 11 文件不可变快照经官方 `review-agent` 返回 `STATIC_PASS`，无 P0-P3；`80 passed, 1 skipped`，仅 APP-origin 子断言因无 materialized APP/lib root 未观测。真实 regular base SIF 仍不存在，pair gate 首个边界保持 `APP_BASE_SIF_PATH_SYMLINK`（`rc=4`）；未进入 SIF/Apptainer、C++/Python runtime、Slurm/Tiger，未上传。证据见 [SIF APP static check](../Experiments/TigerCluster/docs/sif-app-static-check-20260915.md#r51-final-static-closure) 和 `.codex-tmp/spec185-sif-app-closure-review-r51/`。
2026-09-15 14:15 -05:00 Local pair gate after checkpoint：提交 `4ca2379d` 的脚本以 Apptainer `1.5.3` 重跑，首个边界仍为 `APP_BASE_SIF_PATH_SYMLINK`、`rc=4`；原始记录 `.codex-tmp/local-first-sif-attempt-20260915-r3/`。未进入 Apptainer、builder、APP 提取或运行，未上传 Tiger；取得 regular base SIF 并核对锁定摘要后才能继续。

2026-09-15 B187-BASE r1 static review：`STATIC_FAIL`，首要边界是 OpenABE/RELIC 仍可从默认 base 库路径加载，另有 ELF 来源、旧入口、Python 路由和构建身份覆盖不足。已修复并冻结 r2 复审，尚未运行构建。唯一证据见 [base repair](../specs/187-yolo-minindn-sif-app/evidence/b187-base-repair.md)，快照位于 `.codex-tmp/base-repair-20260915/review-r1/` 与 `review-r2/`；失败不计为运行 PASS。

2026-09-15 B187-BASE build run-r1：父 SIF 已通过全文件 SHA-256、容器启动与展开，NumPy wheel 重装成功；首次失败为编译前 `/usr/bin/g++` 不存在（Apptainer rc=255）。父镜像是 runtime，不能假定含编译器。修复为候选容器内安装系统工具链/开发包，r4 静态复审后用新运行目录重试。原始日志 `Experiments/TigerCluster/images/base-repair-20260915/run-r1/build.log`、FAIL record 和 [批次证据](../specs/187-yolo-minindn-sif-app/evidence/b187-base-repair.md) 保留；未产生可验收 SIF。

2026-09-15 B187-BASE legacy gate check：旧 `test_build_local_sif_record.py` / `test_spec170_exact_sif_gate.py` 为 6 failed、11 passed；首边界是 fixture 未满足既有 validator 的 `NDNSF_NAC_ABE_PREFIX=/opt/ndnsf-stage` 标记，导致预期后续检查无法到达。相关旧代码/测试相对 HEAD 均未修改，且没有调用新 base 入口；作为旧入口 fixture 修复事项保留，不假报全套通过。原始 `.codex-tmp/base-repair-20260915/existing-builder-tests.log` 和 [base evidence](../specs/187-yolo-minindn-sif-app/evidence/b187-base-repair.md)。

2026-09-15 B187-BASE run-r2 resolution：补齐容器工具链后，实际 SIF 构建、镜像内 C++ 编译/运行、NFD/NumPy/ELF 检查及最终 SIF 复验通过。SIF SHA-256 `7b4b501033f2db876ccf5c19a9637a58b8b232cf4d555f5b8a225638e180837c`；临时 overlay 删除 OpenBLAS 后正确拒绝，原 SIF hash 未变。r1 缺少 g++ 边界已解决；旧六项 fixture 失败仍保留。仅 `BASE_SMOKE_ONLY PASS`，不是 APP/MiniNDN/Tiger PASS；见 [durable receipt](../specs/187-yolo-minindn-sif-app/evidence/base-smoke-20260915.json)。
2026-09-16 B187-CONTAINER-UNIT failure：候选 SIF `e6cef05a…541657` 内 C++ 测试已完成编译和 ELF 加载，`unit-r1` 首个运行边界因 `--containall --no-mount home` 下 fixture 无法创建 `/home/tianxing/.ndn` 而返回 `rc=134`；原始记录 `.codex-tmp/spec187-clean-restart/unit-r1/`。不是协议结果或库缺失；驱动经静态复审补充独立 `0700` HOME 后，`unit-r2` C++ DI/YOLO smoke 通过。
2026-09-16 B187-YOLO-NATIVE failure：候选 SIF 内 ORT 和 NDNSF-DI runner ELF 均已加载，`yolo-r1` 首个运行边界同为不可写 `/home/tianxing/.ndn`，返回 `rc=134`；原始记录 `.codex-tmp/spec187-clean-restart/yolo-r1/`。驱动经静态复审补充独立 `0700` HOME 后，`yolo-r2 --native-runner` 三次推理通过；两次失败均保留，不计为模型或协议 PASS。

2026-09-16 B187-LOCAL-YOLO request-input transport boundary：host MiniNDN r21 将 6,555,921-byte request input 作为单个 Data 发布，编码包超过 8,800-byte 限制，Provider 未进入完整请求链；原始记录 `.codex-tmp/spec187-local-yolo-r21.log`。按 NDN segmenter/fetcher 契约改为 4,096-byte encrypted segments、`FinalBlockId`、`CanBePrefix` fetch 和总量/顺序/过期校验，未放宽单包限制。

2026-09-16 B187-LOCAL-YOLO selector evidence boundary：分段修复后的 r27 已完成真实 ACK/Selection/ORT CPU/response，但 Python runner 首次以 `CASE_RUNTIME_NATIVE_SELECTOR_RESULT_INVALID` 结束；C++ selector 已写出 `SPEC187_NATIVE_REQUEST_PASS`，因 Boost.Test 默认 ANSI 前缀而未被严格 `startswith` oracle 观察。原始记录 `.codex-tmp/spec187-local-yolo-r27-test-matcher-fixed.log`，通过新增 `--color_output=no` 修复并保留严格标记判据。

2026-09-16 B187-LOCAL-YOLO repeat preflight boundary：r28 第二次运行的首次尝试在 MiniNDN 启动前因复制运行目录时引用不存在的 envelope key 返回 `REQUEST_ENVELOPE_KEY_UNAVAILABLE`；未产生协议结论。原始记录 `.codex-tmp/spec187-local-yolo-r28.log`，随后复用同一 candidate key、隔离状态/输出目录重跑，r28 通过。

2026-09-16 B187-LOCAL-YOLO maintained-runner compatibility boundary：r33–r35 将启动失败定位到旧 MiniNDN `popenGetEnv()` 对含 `=` 的环境值使用无界 `split`；r38 仅用临时 shim 证明替换解析后可完成真实链。维护 runner 已加入进程内兼容层，并由 Python 回归测试覆盖 `util` 与 `application.getPopen` 两入口。r39 使用维护脚本完成 host MiniNDN Y-A，首个协议边界为 terminal response，`SPEC180_CASE_RESULT status=PASS case=Y-A`；原始记录 `.codex-tmp/spec187-local-yolo-r39.log`，持久证据见 [local YOLO recheck](../specs/187-yolo-minindn-sif-app/evidence/b187-local-yolo-recheck-20260916.md)。不启动 SIF/Tiger，也不把该 local PASS 外推为集群资格。

2026-09-16 B187-LOCAL-YOLO current-source replay boundary：r40 因新输出根目录未创建返回 `OUTPUT_ROOT_MISSING`，r41 先后暴露输出路径门禁和 `LOCAL_NATIVE_BUILD_REJECTED:PROVIDER_LINKAGE_CHANGED`；按系统优先路径重新生成 native receipt 后 `SPEC180_NATIVE_IDENTITY_OK`，r42 在 MiniNDN Controller 控制阶段于证书注册边界以 `corrupted size vs. prev_size` abort。原始记录 `.codex-tmp/spec187-local-yolo-r40.log`、`r41.log`、`r42.log` 及 `results/spec187-local-yolo-r42/controller.log`；未进入 ACK/Selection/Provider，保留 r39 历史 PASS，不计当前重放为协议 PASS，待最小化 native 崩溃诊断。
最小化 Controller 绑定复现可构造对象但在显式删除时 `SIGSEGV`，GDB 顶层为 Certificate map 析构；这是当前 native 生命周期诊断边界，尚未归因或修复。

## 2026-09-16 request-scoped segmented-input regression

当前源码已将 6,555,271-byte request-scoped input 从单个 Data 改为每段 4,096-byte
加密 Data；为满足 ndn-cxx `SegmentFetcher` 的 `prefix/version/segment` 契约，User
与 Provider 的输入基础名同步追加 `.appendVersion(attempt)`。官方静态复审快照
`.codex-tmp/review-segmented-input-20260916-r3/` 返回 `STATIC_PASS`，changes
SHA256=`1ca6d5017d0ffd0d8990bbd23dfb033fcc7cb1122432c0ca49af5042c210e020`。

首次运行 `RequestScopedSelection/*` 曾出现一次旧 fixture 5s timeout，边界记录在
`.codex-tmp/spec187-segmented-input-r1/request-scoped-suite.log`，未覆盖源码异常，
也未计为 PASS。单独重跑和第二次整套 selector 均 `rc=0`；长输入 C++ 回归实际
观察到 1,601 segments、统一 FinalBlock、单包 wire <8,800 bytes，Provider
SegmentFetcher 在 handler 前完成组装。相关结果见
`specs/187-yolo-minindn-sif-app/evidence/b187-local-yolo-recheck-20260916.md`。

2026-09-16 B187-LOCAL-YOLO repeated replay：复用同一 candidate-bound
config/input、native selector 和匹配的 NDN-SVS build。第一次使用普通用户创建的
state/output 根，在 MiniNDN 启动前以 `STATE_ROOT_OWNER_MISMATCH` 返回 78，原始日志
`.codex-tmp/spec187-local-yolo-1789595123000000000.log` 保留；修正目录 owner 后
同一 run 完成 ACK、Selection、Provider execution 和 terminal response，退出码 0，
native result 7,267 bytes，未留下子进程。该重试再次确认 host MiniNDN 正向链；SIF、
APP、negative path 和 Tiger 未执行。持久证据见
`specs/187-yolo-minindn-sif-app/evidence/b187-local-yolo-recheck-20260916.md`。

2026-09-16 B184-QWEN-REAL local replay：真实 Qwen3-0.6B 候选的 r1–r4 依次暴露
conversation input contract、tensor bundle rank/shape 和 wrapper/native digest CLI
边界，均已保留原始 run。修正 bundle 为 Int64 `input_ids`、rank 2、shape `[1,n]` 后，
r5 中 Controller、Authority 和三个 Provider 均 ready，Provider 发出 V3 offer；
Requester 随后在 `ACK_CLOSED` 以 `DI_NATIVE_REQUEST_CANCELLED_OR_EXPIRED` 结束，
未到 Selection、Provider execution 或 terminal response。请求 deadline 为 180 s，
本次不计 Qwen PASS，首个待诊断边界为 post-ACK canonical source/initializer
publication/assembly 的耗时与资源。原始记录位于
`results/spec184-qwen06b-local/qwen06b-local-real-r5/`，持久说明见
`specs/184-native-di-closure/evidence/qwen06b-local-real-replay-20260916.md`。

2026-09-16 B187-LOCAL-YOLO corrected-dependency replay：r42 的旧 NDN-SVS 环境被确认不是当前协议边界；复用同一 candidate-bound 输入并将依赖路径固定到 `ndn-svs/build-spec187-local` 后，新的 root-owned run `spec187-local-yolo-1789593707473878369` 返回 `SPEC180_CASE_RESULT status=PASS`，观察到 `ACK_CLOSED`、`SELECTION_COMMITTED`、`SELECTION_ACCEPTED`、`PROVIDER_EXECUTION_COMPLETED`、1,601 个分段输入、7,267-byte 终端结果和完整子进程清理。原始记录 `.codex-tmp/spec187-local-yolo-1789593707473878369.log` 与 `results/spec187-local-yolo-1789593707473878369/` 保留。本机 Y-A 正向链通过；未运行 SIF/APP、negative path 或 Tiger，不改变 T001/T003 的 `PARTIAL` 或 T004 的 `WAITING_EXTERNAL_INPUT`。

该回归只闭合了 C++ DummyFace 的分段发布/组装边界；当前源码 MiniNDN r42 的
Controller `corrupted size vs. prev_size` 控制面崩溃仍是独立未修复边界，不能以本次
selector 结果替代当前源码 MiniNDN、SIF/APP 或 Tiger qualification。

2026-09-16 B187-LOCAL-YOLO checkpoint boundary：本次四文件本地 checkpoint 首次执行
被现有 `.git/hooks/pre-commit` 拒绝；钩子扫描整个 Git index 中的既有
`.specify/memory/*` 文档引用并返回 exit 1，未绕过门禁。暂存内容已保存到
`.codex-tmp/preexisting-index-20260916.patch`；该边界不影响前述 C++ 构建与 selector
结果。

2026-09-16 B187-LOCAL-YOLO current-source replay：旧 native receipt 先以
`STALE_SOURCES` 拒绝；重新配置首次因锁定 Rust cargo 缺失失败，补入封存 Rust 1.90
工具链后配置成功。一次长编译被外部会话终止，随后发现并删除唯一零字节
`OnnxRuntimeModelRunner.cpp.7.o`；受影响 Native/DI 目标重链成功，最终
`SPEC180_NATIVE_IDENTITY_OK`。第一次直接链接暴露 `registerOnnxRuntimeBackend` 未解析，
边界归因于中断留下的零字节对象，未作为协议失败。原始记录保留在
`.codex-tmp/spec187-local-yolo-current-build-20260916.log`、
`.codex-tmp/spec187-local-yolo-current-configure-20260916.log`、
`.codex-tmp/spec187-local-yolo-current-configure-r2-20260916.log`、
`.codex-tmp/spec187-local-yolo-current-native-build-r3-20260916.log`、
`.codex-tmp/spec187-local-yolo-current-native-build-r4-20260916.log` 和
`.codex-tmp/spec187-local-yolo-current-waf-r5-20260916.log`。

官方 `review-agent` 对 exact-SIF 输出根修复返回 `STATIC_PASS`；host MiniNDN 输出根
仍允许外部目录。r52/r53 在 MiniNDN 前分别因 `OUTPUT_ROOT_NOT_EMPTY` 拒绝，保留原始
记录 `.codex-tmp/spec187-local-yolo-r52.log`、`.codex-tmp/spec187-local-yolo-r53.log`。
随后使用全新 root-owned run-id `spec187-local-yolo-1789592534950657908` 完成本机
Y-A：退出码 0，C++ 日志观察到 ACK_CLOSED、SELECTION_COMMITTED、SELECTION_ACCEPTED、
PROVIDER_EXECUTION_COMPLETED、终端响应和 `SPEC187_NATIVE_REQUEST_PASS`；
6,555,271-byte 输入分为 1,601 个 4,096-byte Data segments，结果 7,267 bytes，
所有子进程均完成清理。该结果是当前源码的 host MiniNDN 正向 PASS，不是 SIF/APP、
negative path 或 Tiger qualification；详见
`specs/187-yolo-minindn-sif-app/evidence/b187-local-yolo-recheck-20260916.md`。
2026-09-16 B187-C++-LARGE-DATA fixture boundary：新增分段发布 selector 首次使用
完整 `useSigningKeyChainForTest` 时，在没有 authority 的 LocalMock 中同步获取 NAC
公共参数并以 `Failed to fetch public parameters after multiple attempts` 失败，原始
日志 `.codex-tmp/spec187-large-data-publisher-20260916-r2.log` 保留。该失败发生在
测试签名前置，不是生产分段协议结果。改用只绑定签名 KeyChain 的测试钩子，并把
事件泵 timeout 改为负值后，selector 以 `rc=0`、`28/28` assertions 通过；证据见
[B187 C++ large-data publisher](../specs/187-yolo-minindn-sif-app/evidence/b187-large-data-publisher-cpp-20260916.md)。
全量 unit/integration 构建的既有首边界仍是缺失 ONNX C++ 头文件，未被本 selector
绕过；Qwen、SIF/APP、negative path 和 Tiger 未执行。

2026-09-16 B184-QWEN-NATIVE-REBUILD dependency/ABI boundaries：复用现有构建树构建
`DI_NativeRequester` 首次在 `NativeOnnxRecipeAssembler.cpp` 因缺少
`onnx/checker.h` 退出，原始 `.codex-tmp/spec187-qwen-budget-build-r1-20260916.log`；
从封存 base SIF 恢复 `/opt/onnx`、离线重建 tokenizer bridge 后，requester `106/106` 和
当前 `integration-tests` `127/127` 构建通过。旧 integration-tests 运行时若将
`.local-boost171/lib` 置于 NAC-ABE 新前缀之前，会加载旧 `libnac-abe.so` 并因缺少
`AttributeAuthority::getPublicParametersVersion()` 在进程启动时退出，原始
`.codex-tmp/spec187-qwen-cpp-focused-r1-20260916.log`；按正确顺序以
`install-spec187-nac-r1/lib` 优先后，`Spec182GrantClientFlow/*` 4/4（34 assertions）和
`Spec170NdnsfDiCoreFlow/Spec184DurableOutcome` 1/1（10 assertions）通过，日志为
`.codex-tmp/spec187-qwen-cpp-focused-r4-20260916.log` 和 `r5`。这些是依赖/运行时 ABI
边界与本机 C++ selector 结果，未启动真实 Qwen、未取得 MiniNDN 数值结果，不能提升 T007。

2026-09-16 B184-QWEN native regression target boundary：当前源码
`RequestScopedSelection/*` 4/4（6,570 assertions）通过，`NativeYoloMergeDecodesAndOrdersDependencyTensors`
21/21 与 `NativeProviderIssuesCanonicalPreparationOfferV3` 10/10 通过；同次运行
`Spec175NativeAssembly/*` 的另外 5 项在 fixture 前置检查处因
`DI_NativeOnnxAssemblyWorker binary not found` 退出。原始记录
`.codex-tmp/spec187-qwen-cpp-yolo-focused-r2-20260916.log`。本轮构建边界只包含
`DI_NativeRequester` 与 `integration-tests`，所以这 5 项为 `UNOBSERVED`/目标未构建，
不是协议或模型失败；未为此启动全量构建或 1.5 GiB Qwen assembly。

2026-09-16 B184-QWEN assembly-worker candidate boundary resolved：设置
`NDNSF_SPEC182_BIN_DIR=build-spec187-local-nac-r1` 后，当前源码
`Spec175NativeAssembly/*` 7/7、141 assertions 通过；此前 5 项的
`DI_NativeOnnxAssemblyWorker binary not found` 仅为测试候选目录未配置。原始日志
`.codex-tmp/spec187-qwen-assembly-worker-focused-r1-20260916.log`。该结果确认 ORT
加载和 1/2/4-provider assembly fixture 可运行，未启动真实 1.5 GiB Qwen assembly。

2026-09-16 B184-QWEN real r10 resource boundary：当前 requester、assembly worker 和真实
Qwen3-0.6B material 启动 MiniNDN 后，Controller/Authority/three Providers ready 并发出
`DI_PLACEMENT_V3_OFFER`；requester 未产生任何 terminal marker。运行期间 requester RSS
约 9.1 GiB、swap 接近 8 GiB，可用内存约 0.5 GiB；为保护主机停止，run record
`results/spec184-qwen06b-local/qwen06b-local-real-r10-current/run-record.json` 记为
`minindn=PASS`（仅启动）、`workload=FAIL`、return code `-15`，停止瞬间记录的
`OWNED_PROCESS_ALIVE` 已在随后清理确认。首个诊断边界为 canonical source/initializer、
encrypted envelope、SegmentFetcher assembled buffer、assembly worker 输入及 requester IMS
的整对象持有和复制放大；这不是 6.55 MB request input 单 Data，已有 segmented-input
selector/YOLO 正向证据。保留原始 run、模型和构建树；资源修复和小对象 C++ 回归前不再
启动真实 Qwen。

2026-09-17 B184-QWEN ownership repair focused exit：官方 review-agent 对
`sourceRefFor`、move 传递和 `CanonicalSourceScrubber` 返回 `STATIC_PASS`；复用当前
构建树以 `/usr/bin/g++ -B/usr/bin -j4` 构建 `DI_NativeRequester` `106/106` 成功。随后
`RequestScopedSelection/SelectedProviderReceivesSegmentedEncryptedInput` `1/1`
（6,570 assertions）及 `Spec175NativeAssembly/*` `7/7`（141 assertions）通过，日志为
`.codex-tmp/spec187-memory-ownership-segment-selector-r1-20260917.log` 和
`.codex-tmp/spec187-memory-ownership-assembly-selectors-r1-20260917.log`。该结果只关闭
小对象 C++ ownership/segmentation 回归，不改变 r10 的真实 Qwen `RESOURCE_BOUNDARY`，
未重启 1.5 GiB 模型。

2026-09-17 B184-QWEN selector setup boundary：重建后首次把
`Spec182OnnxWorkerProtocol/*` 传给 `integration-tests`；该 suite 属于
`unit-tests`，Boost.Test 在执行断言前返回
`no test cases matching filter or all test cases were disabled`。原始输出
`.codex-tmp/spec187-memory-ownership-worker-selector-r1-20260917.log` 已保留。
这是目标选择错误，不是产品、协议或模型运行结果；下一步构建并运行正确的
`unit-tests` selector。

2026-09-17 B184-QWEN segmented-worker regression boundaries and focused exit：首次完整
`unit-tests` 链接因 `tests/wscript` 漏列 `ndn-service-framework/OperationRuntime.cpp`
在最终链接阶段出现 `OperationRuntime`/`OperationSubscription` 未定义；补入该源后
增量 `203/203` 构建成功。首次把 `Spec182OnnxWorkerProtocol/*` 传给
`integration-tests` 返回 `no test cases matching filter`，首次大数据筛选遗漏
`GenericDynamicApi/` 套件同样返回 `rc=200`；另一次分段 selector 使用拼错的
NAC-ABE 路径在装载阶段返回 `127`。这些边界均未进入产品断言，原始日志保留在
`.codex-tmp/spec187-memory-ownership-worker-selector-r1-20260917.log`、
`.codex-tmp/spec187-memory-ownership-large-data-selector-r1-20260917.log` 和
`.codex-tmp/spec187-memory-ownership-segment-selector-r2-20260917.log`。

修正测试目标、夹具二进制和库路径后，C++ `Spec182OnnxWorkerProtocol/*` `30/30`、
`GenericDynamicApi/PreparedAndMessages/LargeDataPublicationEmitsBoundedFinalizedSegments`
`1/1`、`Spec175NativeAssembly/*` `7/7` 与
`RequestScopedSelection/SelectedProviderReceivesSegmentedEncryptedInput` `1/1` 均
`rc=0`。这只关闭本地 worker/分段回归；真实 Qwen r10 的
`RESOURCE_BOUNDARY`、SIF/APP、negative path 与 Tiger 资格状态不变。构建和正确
selector 日志分别为 `.codex-tmp/spec187-memory-ownership-unit-build-r2-20260917.log`、
`.codex-tmp/spec187-memory-ownership-worker-tools-build-r1-20260917.log`、
`.codex-tmp/spec187-memory-ownership-worker-selector-r2-20260917.log`、
`.codex-tmp/spec187-memory-ownership-large-data-selector-r2-20260917.log`、
`.codex-tmp/spec187-memory-ownership-assembly-selector-r2-20260917.log` 和
`.codex-tmp/spec187-memory-ownership-segment-selector-r3-20260917.log`。

2026-09-17 B184-QWEN checkpoint boundary：按规则尝试只提交本次独立的
`tests/wscript` 一行源闭合修复，但既有 pre-commit 全量扫描 Git index 中其他会话的
`.specify/memory` 开发辅助引用并返回 `exit=1`；未使用 `--no-verify`，没有新 commit，
也没有改变其他暂存内容。原始输出为
`.codex-tmp/spec187-memory-ownership-checkpoint-r1-20260917.log`，SHA-256
`47d747b672fd3dfdf392739c5b566c51c2b0bd8c868cb5bc57f05b794df6b886`。

2026-09-17 B184-QWEN large-fetch ownership regression：首次运行
`Spec175NativeAssembly/*` 在 fixture preflight 因未设置
`NDNSF_SPEC182_BIN_DIR` 返回 `rc=201`，未进入五个 assembly worker 用例；原始日志为
`.codex-tmp/spec187-large-fetch-selector-20260917-r1.log`。设置当前 worker
目录并预加载匹配 NAC-ABE 后，`Spec175NativeAssembly/*` `7/7` 和
`RequestScopedSelection/SelectedProviderReceivesSegmentedEncryptedInput` `1/1`
均 `rc=0`。该 setup 边界不计为产品失败；真实 Qwen r10 的
`RESOURCE_BOUNDARY` 未重跑。受影响构建及 selector 日志见
`specs/184-native-di-closure/evidence/qwen06b-local-real-replay-20260916.md`。
2026-09-17 large-fetch ownership checkpoint boundary：隔离 `ServiceProvider.cpp`
checkpoint 仍被既有 pre-commit 全索引门禁拒绝；索引中的其他会话
`.specify/memory/*` development-assistant 引用触发拒绝。未使用 `--no-verify`，
没有新 commit；源码、构建和 C++ selector 证据保持在工作区及 `.codex-tmp/`。

2026-09-17 B184-QWEN r11 preflight boundary：以 root 启动新 Qwen replay 时遗漏维护的
`PYTHONPATH`，`canonical_source_info` 无法导入 ONNX，返回
`MODEL_ONNX_VALIDATOR_UNAVAILABLE`；MiniNDN、请求链和模型均未启动。补入同一
repository wheel 路径后 user/root import 均通过；r11 run 保留为
`NOT_EVALUATED`，不得计作失败请求或 PASS。wrapper 原始输出的 SHA-256 为
`10161caf848a0a88ce4bad47ecbd971191e9bf26723711300dee62dfcc3029db`；下一次使用新
run ID 重试，r10 `RESOURCE_BOUNDARY` 保持不变。

2026-09-17 B184-QWEN r12 preflight boundary：root 环境把
`LD_PRELOAD=libnac-abe.so` 传给 `ldd -r`，造成 ldd 自递归；r12 在 MiniNDN 前被
停止，run record 保留 `NOT_EVALUATED`。无 preload 的 `ldd -r` 探测成功，原始输出
`.codex-tmp/ldd-preflight-no-preload-r12.log`，SHA-256
`696c0e079fd9090ed0b0bd04c7110694b49f59d86495b8bdd7b1950a7cb46272`。

2026-09-17 B184-QWEN r13 resource boundary：去掉 root preload 后 MiniNDN 的
Controller、Authority、三个 Provider 和 requester 均启动；约 119.1 秒时进程树
RSS 达到约 7.1 GiB、`MemAvailable` 低于 2 GiB 安全线，随后以 root 权限停止独立
进程组。未观察 Selection、Provider execution、terminal response 或 numerical oracle；
wrapper 在外部停止前未完成 cleanup，run record 保留 `RUNNING`，分类为
`RESOURCE_BOUNDARY` 而不是 PASS。监控原始日志为
`.codex-tmp/spec187-qwen-r13-monitor-20260917.log`，SHA-256
`3b8349630563308e6a7ea765f0f15a6562395c6c95cf3f39d6d66f75624c7bb6`。停止后磁盘仍约
43 GiB 可用、`MemAvailable` 约 8.2 GiB，所有模型和结果文件均保留。

2026-09-17 B184-QWEN large-fetch WireEncode repair：review-agent 对冻结的
`ServiceUser.cpp` 快照返回 `STATIC_PASS`；将完整 `WireEncode()` 复制改为共享
`ndn::Block` 分段后，受影响 native closure `233/233` 构建成功，分段 selector
`1/1`、`Spec175NativeAssembly/*` `7/7` 通过。运行 RSS、取消延迟和真实 Qwen
workload 仍未观测；下一次 replay 必须使用新 run ID 并保留资源监控。原始日志见
`.codex-tmp/spec187-large-fetch-wire-block-build-20260917-r1.log`、
`.codex-tmp/spec187-large-fetch-wire-block-segment-selector-20260917-r1.log` 和
`.codex-tmp/spec187-large-fetch-wire-block-assembly-selector-20260917-r1.log`。

2026-09-17 B184-QWEN r14 monitored resource boundary：使用 WireEncode 共享
`ndn::Block` 修复和去除 root preload 后，MiniNDN 的 Controller、Authority、三个
Provider 和 requester 均启动，Provider 均发出签名 V3 offer；约 117.0 秒时监控记录
进程树 RSS `6,737,232 KB`、`MemAvailable=1,651,808 KB`、swap
`3,966,452 KB`，低于 2 GiB 安全线，随后 wrapper 返回 `rc=137`。requester 只记录
`CANCELLED`，没有 Selection、Provider execution、terminal response 或 numerical
oracle；run record 保留 `RUNNING` 是因为外部停止先于 wrapper cleanup，分类为
`RESOURCE_BOUNDARY`。监控器留下的精确 r14 role PIDs 已另行终止并核验退出；停止后
`MemAvailable` 约 7.5 GiB，磁盘约 43 GiB 可用。原始监控为
`.codex-tmp/spec187-qwen-r14-monitor-20260917.log`（SHA-256
`57230b6a303986e1b99ed6c14b96f0c1cc2892fb9be38ab45430d1b1ba38a77b`）；该结果不计为
T007 PASS。下一次修复必须改变大对象发布的内存边界（有界或 file-backed segment
serving）并先通过 C++ 资源探针；不得只提高阈值、重复同一 in-memory publisher，或
删除模型、当前构建和原始证据来制造资源 PASS。

2026-09-17 Spec188 B188-1 first selector run：定向构建已完成 `46/46`，但首次运行暴露三
个夹具/环境边界，未进入 Spec188 产品资格。`Spec188RepoFileBackend` 因 selector 构造的
`ArtifactReference.publisherIdentity="repo"` 不是 canonical absolute NDN name，返回
`artifact-invalid-name`；`Spec188RepoMemoryBudget` 因 filesystem authority 的父目录尚未
建立，ownership lock 返回 `repo-persistence-lock-open: No such file or directory`；既有
`DistributedRepoTieredCacheTest` 在同一 sqlite path 同时创建多个 authoritative owner，
返回 `repo-persistence-owned`。原始运行日志保留于
`.codex-tmp/spec188-b188-1-20260917/Spec188RepoFileBackend.log`、
`.codex-tmp/spec188-b188-1-20260917/Spec188RepoMemoryBudget.log` 和
`.codex-tmp/spec188-b188-1-20260917/DistributedRepoTieredCacheTest.log`；本轮分类为
`FIXTURE_BOUNDARY`，不得计作产品 PASS。下一次重试使用新日志并核对 canonical publisher、
父目录初始化和独立 authority 生命周期。

2026-09-17 Spec188 B188-1 second selector run：重建 `6/46` affected tasks 成功；
`Spec188RepoFileBackend` 已通过 `SPEC188_REPO_FILE_BACKEND_OK`。`Spec188RepoMemoryBudget`
首个小对象写入超过其 filesystem vector compatibility threshold（80-byte fixture 对
64-byte threshold），返回 `repo-large-object-vector-path-disabled`；这是 selector 配置
边界，range path 尚未开始。既有 `DistributedRepoTieredCacheTest` 在 lru store 存活时再
创建 oversized store，共用 sqlite authority lock，返回 `repo-persistence-owned`；这是
新增 ownership gate 后测试未隔离独立 authoritative lifetimes 的夹具边界。原始日志保留于
`.codex-tmp/spec188-b188-1-20260917-r2/Spec188RepoFileBackend.log`、
`.codex-tmp/spec188-b188-1-20260917-r2/Spec188RepoMemoryBudget.log` 和
`.codex-tmp/spec188-b188-1-20260917-r2/DistributedRepoTieredCacheTest.log`；本轮分类为
`FIXTURE_BOUNDARY`，不得计作产品 PASS。下一次使用新日志并核对 threshold boundary 与
每个 compatibility selector 的独立 authority path。

2026-09-17 Spec188 B188-1 ASan/UBSan preflight/build boundary：隔离 sanitizer 配置通过
系统 g++ 的 address/undefined 支持检查，但仓库全局 Waf 配置要求 pinned Rust tokenizer
cargo；本机该依赖缺失，首次配置在 `Pinned Rust cargo is missing` 停止。为仅观察 Repo
targets，配置阶段使用 `/tmp/spec188-fake-rust` 占位 cargo（不进入任何 product target），
随后实际 sanitizer build 仍展开共享 Core 依赖；在 `13/46` 编译、`-j2` 时
`MemFree` 约 250 MB 且持续 swap-in/out（约 `48/100`），因此主动停止。原始配置/构建日志
为 `.codex-tmp/spec188-b188-1-asan-20260917-r2/configure.log`（SHA-256
`66aafa631665e308d128eb356c7dc895eecaed0ee76d52df88c418e47d694333`）和
`.codex-tmp/spec188-b188-1-asan-20260917-r2/build.log`（SHA-256
`acb071b125b071f8c4fb9bfbf09b99316ba352baf6f6fcb90b1ef577a48d2599`）。本轮分类为
`RESOURCE_BOUNDARY`/`TOOLCHAIN_BOUNDARY`，无 sanitizer 运行结果，B188-1 保持 `PARTIAL`。
2026-09-17 Spec188 B188-3 initial targeted build：`../waf build
--targets=spec188-model-preparation-publication -j4` 在 selector fixture 编译阶段停止，
因为 `spec188-model-preparation-publication.t.cpp` 未包含 `ServiceUser.hpp` 与
`ndn-cxx/name.hpp`，导致 `PreparedServiceRequest`、`LargeDataPublishResult` 和
`ndn::Name` 类型不完整。未生成 selector、未运行行为测试；本轮分类为
`FIXTURE_BOUNDARY`，原始边界记录见
`.codex-tmp/spec188-b188-3-20260917/build-initial-failure.log`，修复后必须使用新日志和
新快照重试。

2026-09-17 Spec188 B188-3 initial selector：定向构建已成功，但从既有 build tree 启动
`spec188-model-preparation-publication` 时，两个测试均在夹具初始化阶段因仓库相对路径
找不到 `tests/fixtures/spec182/dependency-probes/extraction-vectors.json` 而停止；没有
进入 publication/cache 行为断言。本轮分类为 `FIXTURE_BOUNDARY`，原始记录见
`.codex-tmp/spec188-b188-3-20260917/selector-initial-failure.log`；修复后使用新日志重试。

2026-09-17 Spec188 B188-3 selector retry 2：fixture 路径修复后进入 native role-contract
校验，但 fixture 漏填 `role.adapterVersion=1`，与 candidate splitter 版本不一致，生产
路径返回 `DI_NATIVE_ROLE_BINDING_MISMATCH`，仍未进入 transport publication。该轮分类为
`FIXTURE_BOUNDARY`，原始记录见 `.codex-tmp/spec188-b188-3-20260917/selector-retry2-failure.log`；
修复后使用新快照/新运行日志。

2026-09-17 Spec188 B188-3 regression selector：新 publication selector 已通过后，从
`build-spec187-local-nac-r1` 直接启动既有 `spec185-preparation`，15 项测试在旧 fixture/oracle
路径初始化阶段失败，未进入 publisher 行为断言。该轮分类为 `FIXTURE_BOUNDARY`，原始日志
见 `.codex-tmp/spec188-b188-3-20260917/spec185-preparation-build-cwd-failure.log`；按其
既有契约从仓库根目录重跑。

2026-09-17 Spec188 B188-4 T006 initial targeted build：`spec188-prepared-request-projection`
在 selector fixture 编译阶段停止，原因是 `NativeModelRef` 不能直接赋值
`NativeModelDescriptor`。未生成 selector、未运行 request projection 行为断言；本轮分类为
`FIXTURE_BOUNDARY`，原始日志见
`.codex-tmp/spec188-b188-4-20260917-t006/build.log`（SHA-256
`173077755dde199930762d5e450ed8fb4dc76051ae596acd49897696ddb2ad73`）。修复为显式基类赋值后
使用新快照和新构建日志重试。

2026-09-17 Spec188 B188-4 T006 stale consumer regression：T006 selector 已以新库通过，但
直接启动旧的 `spec185-preparation` 二进制时，在首个冷准备用例发生 SIGSEGV（rc=201）。
新增 `NativeModelDescriptor::artifactReference` 改变了公共结构布局，旧测试目标未随头文件
重新编译，不能作为产品/回归结果；原始日志见
`.codex-tmp/spec188-b188-4-20260917-t006/spec185-preparation-regression.log`（SHA-256
`88a2015ab7fc924ff90557ee21181f0cbab1d3538fac2b8e46003376983c2908`）。下一步重新编译受
影响的 DI library 和 `spec185-preparation` consumer 后重跑。

2026-09-17 Spec188 B188-4 T007 targeted build v2：组合静态门通过后，`ServiceUser.cpp`
在 `ndn::Buffer(tagSize, 0)` 处因 ndn-cxx Buffer 仅支持单参数 size 构造而编译失败；未生成
T007 selector、未运行行为测试。本轮分类为 `COMPILE_BOUNDARY`，原始日志见
`.codex-tmp/spec188-b188-4-20260917-t007/build-v2.log`（SHA-256
`cf42732055425789b87832181b5507627d019848520db5a538f6866c18aab2f5`）。修复为
`ndn::Buffer(tagSize)` 后必须重新冻结快照、静态复审并使用新构建日志重试。

2026-09-17 Spec188 B188-4 T007 targeted build v3：T007 生产代码已通过编译，但 selector
夹具将 `std::string` 直接传给只接受 `const char*` 的 `ScopedEnvironmentValue`，在测试目标
编译阶段停止，未生成 selector/未运行行为测试。本轮分类为 `FIXTURE_BOUNDARY`，原始日志见
`.codex-tmp/spec188-b188-4-20260917-t007/build-v3.log`（SHA-256
`da2910d3abb53b541674b74ed46dda68729d58d61c558425a4e505a7b33f7bf1`）。修复为保持路径字符串
生命周期并传递 `c_str()`，随后重新静态复审。

2026-09-17 Spec188 B188-4 T007 static review v2：官方 review-agent 在不可变快照
`.codex-tmp/spec188-b188-4-20260917-t007/t007-v2.patch` 发现摘要仍复制完整 plaintext，
且 publication 后异常可能泄漏 reservation/file、旧 TTL 回调可能误删替换对象；本轮分类为
`STATIC_BOUNDARY`，未构建或运行。v3 修复了分块摘要、统一回滚和 publication identity，复审
又发现未注册 publication 的 committed file 未删除；v4 最终 `STATIC_PASS`。各快照、SHA 和
审查回执保留在 `specs/188-model-preparation-disk-backed-memory/evidence/b188-request-serving.md`。

2026-09-17 Spec188 B188-4 composition static review v1：官方 review-agent 只读审查发现
`tasks.md` 将已实现 T007 保留为 `NOT_STARTED`，且 request-serving evidence 使用截断的 T006
快照 SHA，无法独立核对组合身份；本轮分类为 `DOCUMENTATION_BOUNDARY`，未构建或运行。已将
T007 更新为 `PARTIAL`、补全 T006 SHA 与 composition durable hash，并以 v2 快照复审通过。
2026-09-17 Spec188 B188-4 T007 selector v5：修复 selector freshness 后，发布结果成功但
`face.receive` 未观察到任何 file-backed segment；metrics 的 publication/read/hit 均为零，
未进入解密重组断言。本轮分类为 `RUNTIME_BOUNDARY`，原始日志见
`.codex-tmp/spec188-b188-4-20260917-t007/selector-v5.log`（SHA-256
`0ca51e899b9000cce7be8e4c9d5ad48f3b812bf6e487b7dcacd9437750e1fec7`）。下一次增加完整
 FinalBlock 驱动的 segment fetch 与 envelope decrypt oracle，并核对 selector 的 Interest
dispatch；不得把当前失败记为 serving PASS。

2026-09-17 Spec188 B188-4 T007 selector v6：T007 selector 仍未收到 segment Interest，
metrics read/hit 为零。生产静态路径未进入 `onInterest`；根因是 LocalMock 夹具将三参数
`setInterestFilter` 解析为需要 NFD prefix registration 的 `RegisteredPrefixHandle`，Dummy
Face 没有 forwarder，因此本地 filter 没有安装。原始日志见
`.codex-tmp/spec188-b188-4-20260917-t007/selector-v6.log`（SHA-256
`2c73d9fa998820486b55386c4a9df970ddc57b8012dd47374be2bb2511333d22`）。已改为显式两参数
`InterestFilter` overload 并保存 `ScopedInterestFilterHandle`，修复后需重新静态审查、构建和运行。

2026-09-17 Spec188 B188-5 T009 static review v1：官方 review-agent 发现新增 cache lifecycle
用例未被 `di-prepared-process.t.cpp` 的 repeatable native matrix 选中，旧前缀仍指向
`Spec185ProviderAssembly/<case>`；本轮分类为 `STATIC_BOUNDARY`，未构建或运行。随后将
matrix 改为 `Spec185ProviderAssembly/Spec188ProviderReferenceAssembly/<case>`，并把外层
`ProviderOnlyRuntimeServesAndDrainsNativeRegistration` 单独选取；v3 快照复审通过。

2026-09-17 Spec188 B188-5 T009 selector build-cwd boundary：`spec185-provider-assembly`
完整目标构建已通过，但从 `build-spec187-local-nac-r1` 目录运行 Provider selector 时，
`NdnsfIntegrationEnvironment` 找不到相对路径 `examples/trust-any.conf`，嵌套 selector
返回 201，外层 drain selector 也返回 201；没有进入 Provider 产品断言。原始日志为
`.codex-tmp/spec188-b188-5-20260917-t009/selector-v1.log`（SHA-256
`430db8f95aa94a587c25ca025f1a2fdff3bea3633b02dbd478c3ac3aa0cb193a`），分类为
`FIXTURE_BOUNDARY`。从仓库根目录使用同一二进制和新日志重跑后 13/13 + 1/1 通过。

2026-09-17 Spec188 B188-5 T009 runtime checkpoint：静态 v3 通过、完整 DI closure `-j4`
构建通过，根目录 Provider selector 嵌套 13/13、外层 drain 1/1 通过；T009 两个新用例与
Provider drain 各独立运行两次也通过。构建日志 `.codex-tmp/spec188-b188-5-20260917-t009/build-v1.log`
SHA-256 `52378c22970bbc08aa77215129b97986f756a555a7e62c89d0cf1ca44e661a8a`，selector 日志
`.codex-tmp/spec188-b188-5-20260917-t009/selector-v2.log` SHA-256
`d84b03b8aa3195d962a716409841e5498419390aeb1cc49554c39a593610ae28`，重复日志
`.codex-tmp/spec188-b188-5-20260917-t009/selector-repeat-v1.log` SHA-256
`57b11aa0da103b72e9ce8fa9e29e7e5c219005a1464befbb5031d68156618cf8`。本轮只计 T009
lifecycle focused evidence；真实 revoke、assembly-failure cleanup、非协作阻塞 I/O 取消、
 ASan/UBSan、TSan、range 长对象和临时物化删除时序仍未观测，B188-5 保持 `PARTIAL`。

2026-09-17 Spec188 B188-6 T010 static review：官方 review-agent 对不可变快照
`.codex-tmp/spec188-b188-6-20260917-t010/t010.patch`（SHA-256
`3bba3cfe73ce09ee730440013ddd10ea6527d49d186002da1de5ae75ba4ed95b`）审查通过，未发现
P0-P3 控制性缺陷。审查确认 A/B/C identity 与 digest、custom source deleter、maxEntries=1
eviction、lease pin/release、stop cleanup 及 repeatable matrix 接线；ModelPreparationCache/
Repo/publisher/runner/input 跨层 accounting、约 1.5 GB initializer、RSS/disk/residue 和
sanitizer 仍是未观测边界，未因静态通过而计为完成。

2026-09-17 Spec188 B188-6 T010 compile-link：受影响的 `spec185-provider-assembly` DI
closure 使用 `/usr/bin/g++ -B/usr/bin`、`-j4` 构建通过，耗时 22.49 s，峰值 RSS
1,236,236 KiB，exit 0。原始日志 `.codex-tmp/spec188-b188-6-20260917-t010/build-v1.log`
SHA-256 `ebc4acb8bda0cd8698e05f90d27c248877825b452d5e1a579499354506d528f9`。

2026-09-17 Spec188 B188-6 T010 runtime checkpoint：从仓库根目录使用同一构建二进制，
`Spec185ProviderAssembly/Spec188ProviderReferenceAssembly` nested selector 14/14、
outer Provider drain 1/1 通过；T010 owner probe 独立重复 2/2 通过，观察到 A→B→C 两次
eviction 后 source destructor 计数以及 stop 后第三次析构，active leases 归零。首次运行日志
`.codex-tmp/spec188-b188-6-20260917-t010/selector-v1.log` SHA-256
`4b34fef9318c220cdc1273f797fe06e1cadc9ad147ae0f9def63eb4fcc271eaa`，重复日志
`.codex-tmp/spec188-b188-6-20260917-t010/selector-repeat-v1.log` SHA-256
`2c38a382b20522dde11be634427bbd6ef51eed16be02035a41fb7d3ca763f76e`。本轮仅证明
ProviderArtifactCache owner/eviction focused behavior；跨层 counters、1.5GB RSS/swap/disk/
residue、真实 source closure、TTL/admission、ASan/UBSan 与 TSan 尚未运行，T010/B188-6 保持
`PARTIAL`。

2026-09-17 Spec188 B188-7 T011 preflight：首次重复运行传入的 `--repeat-root`
不在维护的 `results/` 根下，runner 在启动 MiniNDN 前返回
`REPEAT_ROOT_OUTSIDE_RESULTS`；本轮没有协议结果，原始日志
`.codex-tmp/spec188-b188-7-20260917-t011-v11/minindn-repeat-v14.log` SHA-256
`caf07f1a56f6733b2f99b01c536ed2d39bd319357029980e6056ee0b9864bb2d`，分类为
`FIXTURE_BOUNDARY`。修正 output root 后使用新的 run directory 重试，不覆盖该失败记录。

2026-09-17 Spec188 B188-7 dependency closure：Waf 默认排除历史
`.local-boost171`，runner 在 host 模式拒绝从该目录加载 `libndn-cxx`；冻结快照
`.codex-tmp/spec188-native-dependency-closure-review-v1.snapshot` SHA-256
`e3e4477217a785723a51870f460da5b17a5f01865f7f46a0351448be7690cd93` 经官方
review-agent `STATIC_PASS`。受影响 DI closure 用 `/usr/bin/g++ -B/usr/bin`、`-j4`
构建和 verify 通过；native build log SHA-256
`5ad1410a73ae3bf3d908af28f7359021def4840f2d60d592ced1afc81a1f62c1`，耗时 5:33.57，
peak RSS 6,728,764 KiB，Swaps 0。该记录只证明依赖边界和构建，不证明 Spec188
产品完成。

2026-09-17 Spec188 B188-7 T011 runtime：受控 `/usr/local` NDN-CXX host 闭合下，
`Spec188YoloRepeat` 两轮 MiniNDN 均有 ACK、Selection、Provider execution、terminal
response、clean child exit，summary SHA-256
`df8661cecb4965decf97af0034c34bf75ee5e6e589992dbea5d8f26bfe868428`，wrapper log
SHA-256 `5fd2179f75ead5aa98aa2a14a77b5fcd45faa166dc91f6f63f426f93121e0baa`。但两轮
`user.log` 都出现 `LARGE_DATA_PUBLISH_SEGMENTS` 和
`LARGE_DATA_PUBLISH_FILE_BACKED`，所以请求时仍重复发布 canonical source/initializer；
这是 T005/T011 的生产接线缺口，不能把协议链 focused PASS 提升为
`zero model republication` 或资格 PASS。T011/B188-7 保持 `PARTIAL`，T012/T013
未开始；完整边界见 `specs/188-model-preparation-disk-backed-memory/evidence/b188-local-validation.md`。

2026-09-17 Spec188 B188-7 runner regression：完整 Python runner 回归首次为 104/105，
唯一失败是既有 `test_initialize_keychains_uses_case_identity_parent` 仍传裸
`object()`；生产方法在调用 legacy keychain 后按契约访问 `ndn.net.hosts` 重写节点
TPM locator，失败发生在测试夹具边界，没有启动 MiniNDN。修复为带空 `net.hosts` 的
最小 stub 后，冻结快照 `.codex-tmp/spec188-b188-7-t011-test-fixture-review-v1.patch`
SHA-256 `f46e81d34d03eb4123e8e53c526e425300a7ac12f624d73f1ffeff2c88c90c43`，官方只读
review-agent 返回 `STATIC_PASS`；完整回归 105/105 PASS。该修复不改变生产代码，
原失败不计产品行为结果。

2026-09-17 Spec188 B188-3 T005 Spec185 consumer regression：重建 `spec185-preparation`
后，首次运行只剩一个失败，`Runtime::prepare` 在新的 prepare-time publication 边界报告
`DI_NATIVE_ENCRYPTED_PUBLICATION_FAILED: NAC-ABE produced no wrapped large-data MessageKey`。
根因是旧回归夹具没有绑定本地 `REQUEST-LARGE` wrapped-key 状态；原始日志
`.codex-tmp/spec188-b188-3-t005/spec185-preparation-regression-v1.log`（exit 201，SHA-256
`0d40450f1464e1fae3f7d03f85406f1bedcf4325305188898cac208df7f04f8f`）已保留。

2026-09-17 Spec188 B188-3 T005 fixture repair v1：加入 LocalMock ServiceUser、grant/admission
绑定及预置 wrapped key 后，官方静态门发现两个夹具边界：未驱动借用 Face 的 `io_context`，且
Runtime/ServiceUser 可能晚于借用 Face/KeyChain 析构；v2 复审修复 poll/join 和声明顺序。
随后重建回归报告 `wrapped large-data key disappeared before publication`，原始日志
`.codex-tmp/spec188-b188-3-t005/spec185-preparation-regression-v2.log`（exit 201，SHA-256
`e97ec622a577eaad08522fdaa84cd4694185e597ab9d6f7fe1760e27d59f4640`）。根因是 LocalMock
`prepareHybridSendKeyForTest()` 仍只写旧标记，没有建立新 refcount 所需的 service/byId 索引；
该失败仍保留为 fixture boundary。

2026-09-17 Spec188 B188-3 T005 final validation：`prepareHybridSendKeyForTest()` 写入
service-scoped wrapped-key placeholder 后，v3 静态复审 `STATIC_PASS`；受影响 DI closure
`spec185-preparation,spec188-model-preparation-publication` 使用 `/usr/bin/g++ -B/usr/bin`
和 `-j2` 构建成功，`spec185-preparation` 24/24 与 T005 selector 5/5 均 exit 0。最终日志和
限制见 `specs/188-model-preparation-disk-backed-memory/evidence/b188-preparation.md`；真实
Repo ingest/lease、TSan、跨进程 Controller/NFD 仍未观测，不能提升 T005/B188-3 状态。

2026-09-17 Spec188 host dependency guard static review：v1/v2/v3 冻结快照分别被官方
`review-agent` 要求补齐 linker flag/RPATH 检查、Waf runtime-RPATH 接线和符号链接别名解析；
原始快照 `.codex-tmp/spec188-host-ndncxx-guard-20260917.patch`（SHA-256
`a63de99421e96d52ca70ebf70444d52d509d1e41fcb01e13b6a6c2add8d459ed`）、`-v2.patch`
（`09b3fc288791421987688459d028a2fc3ddba42c0209c55d788138ae545dd33b`）和 `-v3.patch`
（`49deb50b343d68180f79445186c16ae5135bf1eb61ae033ed5473c5df5cc7562`）保持不变。v4
快照（`9f8a38782ff03c4c45b79f48c826b65bc85c346ed4065ad7de97c5d49a88ac6f`）补齐
`-Wl,-rpath`、`-rpath-link`、`-R` 及 realpath 解析后通过
`STATIC_PASS`，未构建或启动实验；完整当前证据见
`specs/188-model-preparation-disk-backed-memory/evidence/host-dependency-closure-20260917.md`。

2026-09-17 Spec188 B188-1 sanitizer boundary：第一次 ASan/UBSan Repo 构建虽然带有
sanitizer flags，但复用的 Waf cache 含 `.local-boost171` RPATH，且默认 `ldd` 会加载
`/usr/local` 的普通 Core；该结果不计入验收，日志保留在
`.codex-tmp/spec188-b188-1-asan-20260917-r5/build-repo-j1.log`。第二棵 canonical
`-Og -g3` 树在高内存的 `ServiceUser.cpp` 阶段停止；日志为
`.codex-tmp/spec188-b188-1-asan-canonical-o0-20260917-r3/build.log`。两次边界都没有
伪造 PASS。

随后以默认禁用历史 prefix、系统 Boost 1.71、`/usr/local` NDN-CXX/NFD、`-O0 -g0`
和 `-j1` 重新配置 `build-spec188-b188-1-asan-canonical-o0g0`；46/46 构建成功，
三个 Repo C++ selector 在候选构建树优先的 `LD_LIBRARY_PATH` 下串行通过且无 ASan/UBSan
报告。构建日志 SHA-256 及 selector 日志 SHA-256 见
`specs/188-model-preparation-disk-backed-memory/evidence/b188-repo-file.md`；
TSan、磁盘/权限故障注入和多 pinned 压力仍未运行。

2026-09-17 Spec188 B188-1 TSan preflight：使用 canonical host 依赖和 GCC 9.4.0
配置 `--with-sanitizer=thread` 时，Waf 的 compiler capability check 返回 `no`，随后
以 `thread sanitizer is not supported by the current compiler` 退出，未生成任何对象。
该边界归类为 `TOOLCHAIN_LIMITATION`；配置日志为
`.codex-tmp/spec188-b188-1-tsan-canonical-o0g0-20260917-r1/configure.log`。
按显式路径尝试建立本地 checkpoint 时，仓库既有 index 中的 development-assistant
文件触发 pre-commit 拒绝（commit log SHA-256
`ccdaaebc57a41f9445fb2e4b4560d743d1918f36291294b3f3882213bc97a9c2`）；未绕过、未提交。

2026-09-17 Spec188 host dependency guard positive control：使用 `/usr/local` pkg-config、
匹配的 NDN-SVS source/build、NAC-ABE prefix 和系统 Boost 1.71 配置时，Waf 的 NDN-CXX、
SVS、NAC-ABE、Boost linkage 检查均通过；随后在独立的 pinned Rust cargo 缺失处退出，
没有进入编译或运行。原始日志 `.codex-tmp/spec188-host-canonical-configure-20260917-final.log`
SHA-256 `9399cb3668e27e59364d07aec078830a99c73658cc2b5e4ecba359cc844a683c`，分类为
`TOOLCHAIN_PRECHECK`，不能把该次配置记为完整 configure PASS。

2026-09-17 Spec188 B188-1 multi-pinned selector launch boundary：在 canonical
build tree 内首次重建命令误用 `./waf`，该目录没有 waf wrapper，exit 127；随后从仓库根
启动 selector 时引用了构建树内 `$PWD/.codex-tmp` 的日志目录，目录不存在，exit 1，
均未开始产品执行。原始输出保留于
`.codex-tmp/spec188-b188-1-pinned-stress-20260917-r6/result.txt`；修正为仓库根证据目录
和 `../waf` 后，受影响目标构建 exit 0，sanitizer selector exit 0，结果见
`specs/188-model-preparation-disk-backed-memory/evidence/b188-repo-file.md`。

2026-09-17 Spec188 B188-1 fault selector first boundary：用 `UINT64_MAX` 作为 manifest
size 的 sanitizer 反例先在 ArtifactReference hard limit 处返回
`artifact-limit-exceeded`，没有到达 Repo disk reservation；原始日志为
`.codex-tmp/spec188-b188-1-faults-20260917-r2/selector-asan.log`（SHA-256
`823884be2d4ad6a8d142a6019892533793b1c9c5435fa6efe5f84cce008fb73d`）。修正为不超过
1-PiB hard limit 的 `1ULL << 50` 后，reservation、permission atomic failure 和
orphan recovery 均在 normal 与 ASan/UBSan selector 通过；该首边界不计作产品失败。

2026-09-17 Spec188 B188-1 normal build launch boundary：首次 normal 构建命令把工作目录
写成不存在的大小写路径 `/home/tianxing/NDN/NDN-service-framework/...`，统一 exec 未启动
并返回 `No such file or directory`。修正为实际仓库路径后，`../waf build
--targets=Spec188RepoFileBackend -j4` 在 `build-spec187-local-nac-r1` 完成，结果见
`.codex-tmp/spec188-b188-1-faults-20260917-r3/result.txt`。

2026-09-17 Spec188 documentation check command boundary：沿用过时的
`scripts/verify-spec-kit-sync.py` 路径导致 Python exit 2（文件不存在）；实际同步校验器位于
`skills/speckit-code-design/scripts/verify-spec-kit-sync.py`。使用正确入口并传入仓库、entrypoint
和 personal skill 要求后，结果为 `PASS: 11/11 local entrypoints; personal shared skill=present`；
该路径错误不影响产品构建或运行结果。

2026-09-17 Spec188 B188-2 range sanitizer closure：canonical
`Spec188RepoRangeTransfer` 使用系统 Boost 1.71、`/usr/local` NDN-CXX/NFD 和 `-j1`
构建 exit 0，selector exit 0，未见 ASan/UBSan 报告；输出中的
`fullCopyFallbacks=1` 来自随后故意执行的 legacy vector rejection，range 断言发生在该
probe 前且为 0。原始记录在
`.codex-tmp/spec188-b188-2-range-20260917-r1/result.txt`，parser-fuzz、远端异步和真实
磁盘故障仍未运行。

2026-09-17 Spec188 B188-2 parser-fuzz closure：未改动的 normal `unit-tests` binary
运行 C++ `Spec182ObservedOffer/Spec184NativeParserFuzz`，完成 512 个确定性截断、翻转、
前后缀和结构变异，exit 0、`*** No errors detected`。日志 SHA-256
`d493d12b3b4c3444b338b2b036c75db449cbe38460f233f2b0d4f09205d18be`；该结果只覆盖 native
decoder parser lane，remote async/fault lane 仍未运行。

2026-09-17 Spec188 B188-3 canonical prepare sanitizer link boundary：canonical
`spec188-model-preparation-publication` 的首次 ASan/UBSan 构建使用仅用于 Repo 观察的
0 字节 `/tmp/spec188-fake-rust` tokenizer archive，DI 共享库最终链接报
`ndi_token_*` undefined reference，未生成 selector 或运行结果；原始日志为
`.codex-tmp/spec188-b188-3-preparation-20260917-r1/build-asan.log`（SHA-256
`9bb5f472b8bb9ab7e4338b329c279e4cb7ee88d62fb27474efe094b51078fec8`）。该边界分类为
`BUILD_INPUT_BOUNDARY`，不能归类为产品失败或 PASS。改用现有已验证的 pinned tokenizer
archive（SHA-256 `9e482640470ee560b89cf341b19d9a6f4a82aad97b7cd2d20ed44d9d85a1b463`）
重新配置后，target build exit 0，ASan/UBSan selector 5/5 exit 0；结果日志分别为
`.codex-tmp/spec188-b188-3-preparation-20260917-r1/build-asan-retry.log`（SHA-256
`eaaf000e6e7e2f6d63a0c2aad779f84c3587b08fa35d7e2837fabe6292e62fa7`) 与
`.codex-tmp/spec188-b188-3-preparation-20260917-r1/selector-asan.log`（SHA-256
`fcdb2cf980ad10a2a14252e3ddfe420593c3eacf8412a76ab4000ea6f2cc750e`）。

2026-09-17 Spec188 host dependency closure correction：`/usr/local` 没有匹配本机 Boost 1.71
的头文件/库对；主机应使用 `/usr/include` 与 `/usr/lib/x86_64-linux-gnu` 的系统 Boost，
同时让 NDN-CXX/NFD、NDN-SVS 和 NAC-ABE 的传递 `libndn-cxx` 解析到同一个 `/usr/local`
真实文件。T007 旧 ASan selector 混用 `build-spec187-local` 的 SVS 与 `.local-boost171`
NDN-CXX，最终在 `ServiceUser` 析构出现 invalid SVS vptr；该边界保留为
`ABI_BUILD_RUNTIME_BOUNDARY`，原始日志 SHA-256
`33564813cdb0d18fb2f846d8262c889a8eb743637f65c76d2e7d5174b8ed67fc`，不计作产品失败。
按系统 Boost 1.71 和 `/usr/local` NDN-CXX 重建匹配 SVS 后，DI canonical closure 107/107
exit 0（日志 SHA-256 `6ee6da0b47f212b14099b52d9517c95bd00e6f1c7edaa097c069a61cc61dcaad`）；
T005/T006/T007 对应 ASan/UBSan selector 分别 5/5、3/3、2/2 通过。仍不代表 Repo
prepare/lease、完整 Core-NDN request、TSan、MiniNDN 或 SIF/Tiger 资格完成。

2026-09-17 Spec188 T005 Runtime Repo owner build boundaries：canonical `spec185-runtime` 首次
重建在 `di-runtime.t.cpp` 处因 `NDNSF-DistributedRepo/include` 未加入 target include path
退出（`.codex-tmp/spec188-t005-repo-loader-20260917/build-r4-canonical.log`，exit 1，
SHA-256 `b45167352113a46e82c4cabb5e2dd833f245c2a9fec90d67e8cd798756657abc`）；补齐 include
并经 r5 静态复审后，测试 fixture 的 most-vexing-parse 又在编译处退出（`build-r5-canonical.log`，
exit 1，SHA-256 `2e3d58fdde51a9285e310a8303cdaf60955c13b8e54a8844eeb093ae1a3adf35`）。两者均为
构建接线/测试代码边界，不是产品或 ABI 失败；r6 经官方 review-agent `STATIC_PASS` 后，
matching closure 构建 exit 0，RepoCore lookup/miss-ingest 与 typed lifecycle selector 均通过。

2026-09-17 Spec188 T005 API/PDF documentation gate：`Design/build-api-reference.py
--changed-only` 成功生成 current API inventory/Markdown（328 files、17,742 entries；日志
`.codex-tmp/spec188-t005-repo-loader-20260917/build-api-reference-changed.log`，SHA-256
`180190c674de9e556d7b7035462ffc41a8c7bb4b8795604a0af6a7e76878fc53`），并重建 current/target
PDF（run `.codex-tmp/design-pdf-20260917T213510931391Z`）。随后 `Design/verify.py` 在
`verify-api-reference.py` 子门退出 1：当前工作树包含并行 Spec188 源码漂移、新增 Repo 文件未纳入
实现覆盖清单、source snapshot/behavior coverage 与 provenance 不一致（日志 SHA-256
`54309d5bdee97424ca8001fe9a76954719e575fffa60649a34cf655c6f7b1e83`）。这是文档身份/覆盖门
边界，不是 C++ Repo selector 失败；API/PDF 仅记为生成成功、完整文档 qualification 未通过。

提交 `6a1aaf50` 后重新生成 current inventory 并重建 PDF（`.codex-tmp/design-pdf-20260917T214100268780Z`），
`Design/verify.py` 仍在同一 API 子门退出；post-checkpoint verify 日志 SHA-256
`54309d5bdee97424ca8001fe9a76954719e575fffa60649a34cf655c6f7b1e83`。

2026-09-17 Spec188 B188-1 r9 canonical repair checkpoint：fixture 修复后的 immutable
task/composition 快照分别经官方 `review-agent` 返回 `STATIC_PASS`；canonical
`Spec188RepoFileBackend`、`Spec188RepoMemoryBudget`、`DistributedRepoTieredCacheTest`
三项 B188-1 C++ selector 在同一候选优先、系统 Boost 1.71、`/usr/local` NDN-CXX/SVS/NFD
闭合下串行运行，ASan/UBSan 无报告，均 exit 0。`Spec188RepoRangeTransfer` 作为同轮额外
selector 亦 exit 0。构建命令、峰值 RSS、selector 输出和 SHA-256 见
`specs/188-model-preparation-disk-backed-memory/evidence/b188-repo-file.md#r9-repair-composition-and-canonical-runtime--2026-09-17`。
这次结果关闭 B188-1 当前 compile-link/focused-runtime lane，但不覆盖真实
ENOSPC/short-write/cancel、erase/fsync ambiguous fault 或 TSan；T002/T003/B188-1 仍为
`PARTIAL`。这是一条成功检查点记录，不把 focused selector 提升为 MiniNDN、SIF 或 Tiger qualification。

2026-09-17 Spec188 B188-3 checkpoint boundary：当前 provider 代码、测试和证据完成本批 focused
检查后，使用显式 Spec188 路径创建本地 checkpoint 时被仓库既有 pre-commit development-assistant
index 门禁拒绝；`.specify/memory` 及相关 references 已在 index 中但不属于本批。未使用
`--no-verify`、reset 或清理无关改动，源码和证据保持未提交，后续重新划分 coherent checkpoint。

2026-09-17 Spec188 B188-3 current-provider selector launch boundary：第一次从仓库根启动
`spec185-runtime` 时，重定向目标使用了只存在于 build tree 的相对 `.codex-tmp` 目录，shell 在
创建测试进程前以 exit `1` 失败；没有进入 C++ selector 或产品断言。该次只记为 harness/path
边界，已在 `evidence/b188-preparation.md` 登记；修正为仓库根 evidence 目录后才允许重试。

2026-09-18 Spec188 B188-3/T005 and B188-7/T011 current-candidate gate：本轮先保留三条
边界失败再重试。未用 root 运行 MiniNDN 的 r1 在启动前以 `Mininet must run as root` 退出，
归类为 host preflight；root r2 因 request envelope key 仍归普通用户所有而在
`REQUEST_ENVELOPE_KEY_OWNER_MISMATCH` 退出，归类为 fixture ownership preflight；修正 key
所有权后 r6 才进入两轮产品运行并返回 `SPEC188_REPEAT_RESULT status=PASS runs=2`。
另一次 r4 target rebuild 因测试断言错误地对 `PreparedModel` 解引用而在 C++ 编译处退出，
不是产品或 ABI 失败；修正为 `Spec185PreparedModelTestAccess::source(prepared)` 后，受影响
target 使用 `-j2`、25.378 s 构建通过。r6 的 wrapper 日志 SHA-256 为
`5fd2179f75ead5aa98aa2a14a77b5fcd45faa166dc91f6f63f426f93121e0baa`，repeat summary SHA-256 为
`049903877024cb353862605767f466f59f1871bb3fe498ff4834db0db6c4f699`；上述 preflight/compile
边界均未被计为产品 PASS 或失败，真实 C++/MiniNDN 结果记录在 Spec188 B188-3/B188-7 evidence。
2026-09-18 Spec188 T006 same-handle validation boundaries：第一次重跑命令把已经绝对化的
build path 再次拼接到仓库根，selector 在启动前以 `SELECTOR_INVALID`/exit `78` 返回，没有
创建 MiniNDN 运行；另一次 manifest wrapper 使用了不存在的旧子命令
`verify-local-runtime`，只在工具参数解析处 exit `2`。两次都归类为 harness/tooling boundary，
未计入产品运行结果。修正路径和命令后，受影响 target `-j2` 构建 exit `0`，`verify` 返回
`SPEC180_NATIVE_IDENTITY_OK`，随后 r8 root MiniNDN 完成两轮、每轮同一 prepared handle 两次
request，wrapper `status=PASS runs=2`；证据见 `evidence/b188-request-serving.md`。
2026-09-18 Spec188 T015/T016 delivery boundary: Spec188-scoped convergence review r5 returned
`STATIC_PASS` after correcting stale CodeGraph and task/evidence status records. The local bounded
core and current candidate YOLO evidence remain valid, while the repository-wide Design/PDF verifier
still sees unrelated parallel source drift; that boundary is recorded as `UNOBSERVED`, not PASS.
T016 handoff is now recorded at
`specs/187-yolo-minindn-sif-app/evidence/spec188-handoff-20260918.md` with
`WAITING_EXTERNAL_INPUT` for SIF/Tiger. No new native build, SIF build, upload or Tiger run was started.

2026-09-18 Spec189 B189-3 two-provider runner r01: the frozen candidate was rejected before
MiniNDN startup with `PROVIDER_BINARY_DIGEST_MISMATCH` because the command-line expected
`di-native-provider` digest contained one extra character. No Controller, Authority, Provider, or
requester process was started. Raw launcher log is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r01/launcher.log`, SHA-256
`b1fd445096e5f0786ffe9c189cfabe32a2077871f471176d5ba9994e19e7e38e`. This is a harness identity
preflight boundary; retry only after recomputing all candidate binary digests.

2026-09-18 Spec189 B189-3 two-provider runner r02: after the binary digest was corrected, the
runner stopped before MiniNDN startup at `MODEL_CANONICAL_SOURCE_NOT_IMMUTABLE`. The canonical ONNX
graph and external initializer were mode `0664`; the runner rejects writable sources before creating
a hard link into the run directory. Raw launcher log is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r02/launcher.log`, SHA-256
`19b3d017526dc7c7630ff240b2657d210581f4c79254073956efc399bd14441a`. This is a candidate-file
preflight boundary; no native process or MiniNDN network was started.

2026-09-18 Spec189 B189-3 two-provider runner r03: after the candidate files were made read-only,
the runner materialized the requester configuration and canonical hard links, then stopped at
`BUILD_RECEIPT_DIGEST_REQUIRED` in its final identity fence because no receipt digest was supplied.
No MiniNDN process was started. Raw launcher log is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r03/launcher.log`, SHA-256
`4450117cec97109bb11480e9145bcebe6621250a5e8f244db3172bc70fd716d9`. The matching receipt digest
is `e722219b33afc680b387a7c7aeef40d12c357377098df35fffd3192b079e6f80`.

2026-09-18 Spec189 B189-3 two-provider runner r04: the real MiniNDN topology started successfully;
Controller, Authority, Provider-0 and Provider-1 reached ready markers and both providers emitted
signed `DI_PLACEMENT_V3_OFFER` decisions for their assigned stages. The C++ requester then stopped
at `ACK_CLOSED` with `native state mapping differs from the source boundary`. The canonical ONNX
object contained only `input_ids`, `attention_mask` and `position_ids`, while the staged Qwen
artifacts and catalog state contract require dynamic `past_key.*`/`present_key.*` tensors. This is
a native preparation/protocol boundary, not a readiness or wrapper result. Requester log:
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r04/requester-0.log`, SHA-256
`1ce55cc631af6d7543076fc3479f95ae8fab85ed896518423153502306431e52`; launcher log SHA-256
`0b3480b3affa56837d802169f9f57eb7cca931aca33f3284f6e9d5f06df8bf1a`.

2026-09-18 Spec189 B189-3 two-provider runner r05: with the dynamic-KV canonical graph, the real
MiniNDN topology again reached both signed provider offers and passed the prior state-boundary check.
The requester stopped at `ACK_CLOSED` with `cooperative extension deadline exceeded`; the generated
request contract still fixed `max_policy_ms` at 5 seconds, which is too small for source-bound
planning over the 7,343-node graph. Requester log is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r05/requester-0.log`, SHA-256
`4f9d60f3e3a1a506e3b75cfd649060b091d887e546892393c9593d638d43a4ed`. This is a bounded native
planning-budget boundary, not a product execution or qualification PASS.

2026-09-18 Spec189 B189-1 dynamic canonical re-export d01: the temporary exporter stopped before
model loading with `ModuleNotFoundError: llm_pipeline_lib` because `.codex-tmp` was omitted from its
module path. Raw log is `.codex-tmp/spec189-qwen-two-provider-20260918/canonical-dynamic-export.log`.
This is an exporter harness path boundary; no model artifact or native runtime result was produced.

2026-09-18 Spec189 B189-3 two-provider runner r06: after the dynamic-KV graph and a bounded
60-second native policy budget, the real MiniNDN topology reached Controller/Authority readiness,
both role-specific Provider preparation markers, and signed `DI_PLACEMENT_V3_OFFER` decisions. The
C++ requester entered `Runtime.open -> User.prepare -> PreparedModel.request` but stopped at
`NATIVE_STREAM_FAILED` because the Provider rejected the request-scoped stream grant. No ONNX
execution, cross-Provider hidden-state handoff, terminal stream event, or checkpoint was observed.
Requester log is `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r06/requester-0.log`,
SHA-256 `8bb808e73251e152741913f10955f290fbdc05dbd2359149fcbb291a1ec1fc27`. This is a native
stream-grant binding boundary; the next candidate includes diagnostic logs before retrying.

2026-09-18 Spec189 B189-3 two-provider runner r07: Core/DI targets were rebuilt after adding
diagnostic rejection logs to `ServiceProvider::initializeStreamPublisher`. The rebuilt provider
reached the same signed two-provider offers and preparation markers, but the requester again
returned `NATIVE_STREAM_FAILED` with `request-scoped stream grant rejected`. The default child log
level did not emit the new diagnostic line, so the exact verifier subreason remains unobserved;
the next retry enables `NDNSF_NDN_LOG=*=ERROR`. Requester log SHA-256 is
`8bb808e73251e152741913f10955f290fbdc05dbd2359149fcbb291a1ec1fc27` (same bytes as r06), and
Provider-0/Provider-1 log hashes are `cbc7ac22076611e870a29a1d9dc19e77d1103328dc325f95a012b2f3ac8fed77`
and `931543ac88c9d848f7441c2908df0459b636500f647ad7e25da5ae79a937316f`. This is not execution
or qualification PASS.

2026-09-18 Spec189 B189-3 two-provider runner r08: the retry with `NDNSF_NDN_LOG=*=ERROR` did not
reach stream verification. Requester preparation stopped at `PREPARATION_FAILED` with `large-data
file publication has insufficient reserved disk space`; stale 1.5 GB request-publication wire
files from failed earlier runs occupied `/tmp/ndnsf-large-data`. No Provider stream-grant decision
was observed. Requester log SHA-256 is
`b44420c55349cf8e7cbaccd5e28f2dab73b96d720841f010a41c3252a2de85ab`; the stale files were removed
only after all run processes had exited. This is a local resource/preparation boundary, not a
product or qualification PASS.

2026-09-18 Spec189 B189-3 two-provider runner r09: after clearing the stale publication file and
running with `NDNSF_NDN_LOG=*=ERROR`, the requester again entered the native request route and
failed with `NATIVE_STREAM_FAILED` / `request-scoped stream grant rejected`. Both Providers had
reached role-specific preparation and signed `DI_PLACEMENT_V3_OFFER` decisions. The rebuilt
Provider still emitted no diagnostic subreason, so the exact verifier branch remains unobserved;
no execution or hidden-state handoff was observed. Requester log SHA-256 is
`8bb808e73251e152741913f10955f290fbdc05dbd2359149fcbb291a1ec1fc27`; Provider logs are
`0fdc8ee0897a9b5e2357122848d458cf4490f8a9465261709939a4bb0611c073` and
`12bc0e0d31f7bacecf235edeeed351e150345cd3ac3043df73d45c1f83ac662d`. This is not a product or
qualification PASS.

2026-09-18 Spec189 B189-3 two-provider runner r11 preflight: the retry command supplied raw
hexadecimal digests while the maintained runner contract requires the `sha256:` prefix. It
stopped immediately with `MODEL_STAGE_MANIFEST_DIGEST_MISMATCH`; no run directory, MiniNDN
process, Controller, Authority, Provider, or requester was started. Raw invocation record is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r11-preflight/launcher.log`.
Its SHA-256 is `ef99cc72170e02b1df1aed66b8cddb32b7749d52539ec20d1acae8de94c79d0e`. This is a
command-contract boundary, not a product or protocol result; retry with the exact prefixed
candidate digests.

2026-09-18 Spec189 B189-3 two-provider runner r12: after the request-scoped collaboration grant
fix, the real topology reached both role-specific preparation markers and signed placement offers,
then the C++ requester stopped at `NATIVE_STREAM_FAILED` with `Provider lacks controller-authorized
collaboration role /LLM/Pipeline/Stage/0`. The generated controller policy incorrectly used the
application root (`/example/ndnsf-qwen06b`) as the role permission prefix instead of the service
name (`/AI/LLM/Pipeline/QwenNative/ROLE/...`). No ONNX execution, hidden-state handoff, terminal
event, or checkpoint was observed. Requester log is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r12/requester-0.log`; Provider
logs retain the signed offers. This is an experiment policy wiring boundary; fix the generated
policy before retrying, and do not classify the offers as product PASS.

2026-09-18 Spec189 B189-3 two-provider runner r13: after correcting the service-scoped role
policy, both Providers passed ACK/Selection and entered collaboration execution. Both then
failed while loading the protected-grant operator credential with
`DI_PROTECTED_GRANT_REJECTED: operator file cannot be opened`; the requester observed
`stream event gap exceeded retry budget`. The Qwen runner supplied `authority-public.pem` but
did not generate the required sibling `trust-root-registry-v1.json` and registry public-key
closure consumed by `NativeProtectedGrantCredentials`. No ONNX execution, hidden-state handoff,
terminal event, or checkpoint was observed. Requester/Provider log SHA-256 values are
`aabe3820403570ff39f3e7f42672237f3ebce1eeacc9d378217cc1de9cd1e0c0`,
`6ce536f8194292211326c6d7d7fb523c82bd10f94bd6444e63f56b8a015fc9d1`, and
`d97c7f3aea8fc169bfe79b72dd5dc33a67d90fa87d3ca6b22ec80b62816bd1d0`. This is a candidate
operator-credential closure boundary, not a product or qualification PASS.

2026-09-18 Spec189 B189-3 two-provider runner r14 preflight: the command contained a mistyped
canonical external-initializer digest and stopped at `MODEL_CANONICAL_INITIALIZER_DIGEST_MISMATCH`.
No run directory, MiniNDN process, Controller, Authority, Provider, or requester was started.
Raw invocation record is `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r14-preflight/launcher.log`.
This is a command identity boundary, not a product or protocol result; the next invocation reads
all candidate digests directly from files.

2026-09-18 Spec189 B189-3 two-provider runner r16 preflight: the diagnostic retry supplied a
mistyped `DI_NativeOnnxAssemblyWorker` digest and stopped at
`ASSEMBLY_WORKER_BINARY_DIGEST_MISMATCH`. No MiniNDN process, Controller, Authority, Provider or
requester started. Raw invocation record is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r16-preflight/launcher.log`,
SHA-256 `3aed922f546539326d7ffa089096aaafe5d9589d27df0d377310f5ea22c46490`. This is a command
identity boundary, not a product or protocol result.

2026-09-18 Spec189 B189-3 two-provider runner r15: after adding the authority public-key registry
and rebuilding the Provider, both Providers passed signed ACK/Selection and emitted
`NDNSF_DI_GRANT_VERIFICATION` with `boundary=BEFORE_ASSEMBLY`. Provider-1 then failed on the first
protected inter-Provider dataflow operation with `protected dataflow is not authorized for this
role/endpoint`; the requester reported `NATIVE_STREAM_FAILED` / `stream event gap exceeded retry
budget`. Provider-0 had no successful publish event. No ONNX execution, hidden-state handoff,
terminal event, or checkpoint was observed. Raw logs are retained under
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r15/`; rebuilt provider SHA-256
is `c56a680a6dbd4409bf3a17d8dd25ff69a8c87a1cca6b04c2da5989dde230d459`. This is a protected
dataflow authorization boundary, not a product or qualification PASS.

2026-09-18 Spec189 B189-3 two-provider runner r17 preflight: the corrected command reached the
canonical ONNX identity check but root Python lacked the user-installed `onnx`/`numpy` modules and
stopped with `canonical ONNX identity requires onnx and numpy`. No MiniNDN process started. Raw
invocation record is `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r17-preflight/launcher.log`,
SHA-256 `ad02acf7e144f9699408cc4b07d7e7be041fe721dbc23c72eb21e5ffb76144f6`. This is a local
preflight environment boundary, not a product or protocol result; preserve the user module path
when running the root experiment.

2026-09-18 Spec189 B189-3 two-provider runner r18: preserving the user Python site-packages let
the real MiniNDN topology reach signed ACK/Selection and protected-grant verification on both
Providers. Provider-1 then rejected its first fetch; the diagnostic showed an empty endpoint digest
with `role=/LLM/Pipeline/Stage/1`, `producer=/LLM/Pipeline/Stage/0`, `consumer=/LLM/Pipeline/Stage/1`,
`allowed=0`, and `peer_present=0`. The native generation coordinator rebuilt a legacy plan edge
instead of reusing the authenticated V3 endpoint. Requester returned `NATIVE_STREAM_FAILED` /
`stream event gap exceeded retry budget`; no ONNX execution, hidden-state handoff, terminal event,
or checkpoint occurred. Log SHA-256 values are `02fd269cb7e2f4797cc1804d5d3aae2d1d1bccdccc9de97c75c4e774b5d24c14`,
`90060c414841010cef7129f1dc19a2e85d4164ec72d61bc22f3b3e95dcbfcc9f`, and
`abd42fd41bb1fdc7c3dc8cb34514e3974d0efd32f4438cb05853dd51dfe3de2f`. This is a confirmed
production projection boundary, not a product or qualification PASS.

2026-09-18 Spec189 B189-3 two-provider runner r19: after the coordinator was changed to reuse the
authenticated V3 role projection, both Providers again reached ACK/Selection and protected-grant
verification. The empty-endpoint rejection did not recur, but no dependency fetch/publish, assembly,
ONNX execution, terminal event, or checkpoint marker was emitted before the requester stopped with
`NATIVE_STREAM_FAILED` / `stream event gap exceeded retry budget`. Provider logs ended after grant
verification and shutdown, leaving the next first boundary unobserved. Requester/Provider log
SHA-256 values are `cba00bff2663f65967c01cde1952e37716f4c4837d455e093f61a4e5285eeea8`,
`213754fa508971f49553bf204f929e2827835eca73e6ab6db13f57e46511c14b`, and
`716e1110d1efcbef3f64b22b998ccad91116d3cb91588402ff66e529b10dc8a9`. This is not a product or
qualification PASS; enable native runtime timing for the next retry.

2026-09-18 Spec189 B189-3 two-provider runner r20 preflight: the timing/DependencyObjectTrace
retry stopped before MiniNDN startup at `ASSEMBLY_WORKER_BINARY_DIGEST_MISMATCH`; no process or
protocol result was produced. Raw invocation record is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r20-preflight/launcher.log`,
SHA-256 `b617219169a4dcaaf879e3929c93833e3f4f04aaf53d1a8b8f5754ef6563ddab`. The next invocation
reads the assembly-worker digest directly.

2026-09-18 Spec189 B189-3 two-provider runner r21: the corrected candidate-derived assembly-worker
digest and runtime/dependency diagnostics allowed the real topology to reach signed ACK/Selection
and `NDNSF_DI_GRANT_VERIFICATION` with `boundary=BEFORE_ASSEMBLY` on both Providers. Neither
Provider emitted dependency-fetch, assembly-start, runner-ready, execution-completed or terminal
markers before the requester stopped with `NATIVE_STREAM_FAILED` / `stream event gap exceeded retry
budget`. The logs identify protected-grant verification as the last observed boundary, but do not
identify whether coordinator entry, dependency fetch, callback delivery or stream transport failed
next. No ONNX execution, hidden-state handoff, terminal response, checkpoint or cleanup baseline was
observed. Requester/Provider log SHA-256 values are
`42c94ea5f83bf28a325382c762652407b7fd01d55e9cca6ea17e8f0ce1f4ecea`,
`761d393b7de74cece91a145509d9faf9cf2680b6a7e4bd3bc8630e363ec9bd2c` and
`cff5f5af426ef0db28c22f83e0d52744d950b072bb277c4649fa097d50f1a0d6`. This is a runtime
observability/protocol boundary, not product or qualification PASS. The next retry must emit
provider-side post-grant markers and classify the first missing marker instead of treating the
requester stream gap as the root cause.

2026-09-18 Spec189 host dependency-policy configure: the first fresh configure stopped before
the runtime-RPATH gate because `/usr/local/lib/pkgconfig/libnac-abe.pc` still named the retired
`/home/tianxing/NDN/nac-abe-integration-182/install-spec187-nac-r1` prefix. Waf correctly rejected
that global metadata boundary; no product target was built. The matching NAC-ABE source/build pair
was reconfigured and installed with `CMAKE_INSTALL_PREFIX=/usr/local`, after which
`pkg-config --variable=prefix libnac-abe` returned `/usr/local` and the installed library's
loader closure resolved without checkout paths.

The same configure audit found a malformed global NDNSD pkg-config record:
`/usr/local/lib/pkgconfig/ndnsd.pc` advertised `-I/usr/local/includeabc`, a
nonexistent directory. It was replaced from the NDNSD installation template with
`-I/usr/local/include`; a fresh global configure then completed with no
`includeabc`, checkout, temporary or retired-prefix entries in the Waf cache.

2026-09-18 Spec189 host dependency-policy build invocation: an initial attempt passed `-o` to
`waf build` after the most recent configure had locked the output to the explicit container-RPATH
configuration tree. It began an unintended full compile in
`build-spec189-global-policy-config-container-v1`; the process was terminated before any test or
artifact was accepted. Raw command output is `/tmp/spec189-global-r3-marker-build.log`. This is a
Waf output-selection/tooling boundary, not a product or protocol result. The global-r3 tree was
then reconfigured explicitly and the intended affected-target build completed successfully.

2026-09-18 Spec189 host global-install probe: invoking `./waf -o
build-spec189-b189-3-global-r3 install` from the repository root did not select that already
configured tree; Waf entered `build-spec189-global-policy-config-v3` and began a fresh compile.
The timeout stopped it at 4/108 with no installation or accepted artifact. The follow-up probes
with `--no-lock-in-out` and `--no-lock-in-run/--no-lock-in-top` showed the same locked-output
behavior; raw logs are `/tmp/spec189-install-probe.log`, `/tmp/spec189-waf-r3-probe.log` and
`/tmp/spec189-waf-r3-probe2.log`. This is a build-tool invocation boundary, not a product result.

2026-09-18 Spec189 r3 global install permission boundary: running `../waf install` from the
correct `build-spec189-b189-3-global-r3` directory as the unprivileged user reached the intended
tree but failed writing `/usr/local/lib/libndn-service-framework.so.0.1.0` with `PermissionError`.
Raw output is `/tmp/spec189-waf-cd-probe.log`. The authorized `sudo -n` retry from that same tree
installed Core/DI and the Python binding successfully; the permission failure is retained only as
the first boundary of the failed attempt.

2026-09-18 Spec189 B189-3 two-provider-global-r22: the immutable candidate
bundle passed local preflight, but the maintained MiniNDN runner rejected the
profile before startup because `stageNodes` contained three nodes while the
two-stage manifest contained two stages. No protocol process or network
namespace was started; cleanup was `PASS`. This is a runner/profile contract
boundary, not a product result. The corrected profile was assigned to r23.

2026-09-18 Spec189 B189-3 two-provider-global-r23: the corrected global-closure
candidate started the real MiniNDN topology. Controller, Authority and both
Providers became ready; both Providers emitted signed Placement V3 offers and
`NDNSF_DI_GRANT_VERIFICATION` at `boundary=BEFORE_ASSEMBLY`. The requester then
returned `NATIVE_STREAM_FAILED` / `stream event gap exceeded retry budget`.
No `EXECUTION_ENTERED`, dependency fetch, assembly, runner-ready, execution
completion, terminal event or ONNX hidden-state handoff was observed. MiniNDN
startup and cleanup were `PASS`, workload was `FAIL`; this is the first
unresolved post-grant stream/coordination boundary, not a qualification PASS.
Raw logs remain under
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r23/`.
The 1.5 GiB temporary `/tmp/ndnsf-large-data` wire file was removed after all
processes exited to restore disk space; no raw log was removed.

2026-09-18 Spec189 global-closure helper v6: the pre-Waf cache gate rejected
the configured Waf template `RPATH_ST=-L%s` as if it were a concrete external
path (`WAF_CACHE_RPATH_ST_OUTSIDE_GLOBAL_ROOT`). No Waf target or binding was
compiled in this attempt. The raw output is
`.codex-tmp/spec189-global-policy-review/helper-v6.log`; the scanner was
repaired to skip only this literal template while continuing to reject
concrete non-global `-L` and loader paths. The subsequent v7 helper build,
verify, Repo binding build and candidate preflight passed; this failure is a
tooling-gate boundary, not a product or protocol result.

2026-09-18 Spec189 T005 placement-selector build invocation: running `./waf
build --targets=spec189-placement-oracle -j4` from the repository root entered the
locked default `/home/tianxing/NDN/ndn-service-framework/build` tree, which has no
`spec189-placement-oracle` task generator. No source compilation or test artifact
was produced. Raw output is `.codex-tmp/spec189-t005-placement-build-r1.log`.
This is a Waf output-tree selection boundary; retry from the already configured
`build-spec189-b189-3-global-r3` directory, without changing the source or
accepting this attempt as compile evidence.

2026-09-18 Spec189 architecture/progress audit: r25 remains FAIL. Provider-0
reached GRANT_VERIFIED/EXECUTION_ENTERED/ASSEMBLY_STARTED and was requesting
canonical initializer segments; Provider-1 reached EXECUTION_ENTERED and
DEPENDENCY_FETCH, with no RUNNER_READY/EXECUTION_COMPLETED marker on either.
The stream gap alone is not a root cause. Raw run:
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r25/`.
The audit also found a prepare/partition design conflict, an unclosed real
Repo producer/consumer path, an incorrect total-order log oracle and a late
resource guard. See [audit correction](../specs/189-qwen-two-provider-minindn/evidence/spec189-static-audit-20260918.md#architecture-and-progress-correction).
No new runtime attempt occurred. Resume with the narrowed integration map and
resource guard, then Repo wiring; preserve all unobserved execution/output gaps.

2026-09-18 Spec189 B189-1b producer build r1: the static producer/durable
publication gate passed, but the first affected-target build stopped at the
DI publisher boundary. `NativeRequestControl` was passed where the material
validator requires `NativeAssemblyControl`, and the publisher budget read a
nonexistent `maxAssembledBytes` field. No selector or runtime result from this
attempt is accepted. Raw output is
`.codex-tmp/spec189-b189-1b-build-20260918-r1.log`; the source fix adds an
explicit publication budget and request-owner adapter before retry.

2026-09-18 Spec189 B189-1b producer build r2 and selector boundary: after the type and
budget fixes, the affected `unit-tests` target built successfully from the canonical global
tree with `-j4` in 30.325s; the material publication selector then passed. The encrypted Repo
selector (6 cases) and bounded large-data publisher selector (2 cases) also passed. The Runtime
prepare selector passed after assigning a writable `NDNSF_REQUEST_LARGE_DATA_DIR`. Three older
Spec182 publisher lifecycle selectors still fail with a writable directory: source ownership
does not expire at the expected point, cancellation still observes initializer/root work, and
the real-Core cache assertion does not observe the expected cached data. These failures are
retained as unresolved pre-existing publisher/lifecycle boundaries; they are not evidence for
or against the new Spec189 material producer. Raw outputs are
`.codex-tmp/spec189-b189-1b-build-20260918-r2.log`,
`.codex-tmp/spec189-b189-1b-material-selector-build-r1.log`,
`.codex-tmp/spec189-b189-1b-selector-repo-r1.log`,
`.codex-tmp/spec189-b189-1b-selector-bounded-r1.log`,
`.codex-tmp/spec189-b189-1b-selector-runtime-r2.log`,
`.codex-tmp/spec189-b189-1b-selector-cancel-queued-r2.log`,
`.codex-tmp/spec189-b189-1b-selector-cancel-source-r2.log` and
`.codex-tmp/spec189-b189-1b-selector-core-io-r2.log`.

2026-09-18 Spec189 B189-1b consumer/load: the corrected selected-material reader passed
the read-only review-agent (r3), then the affected `unit-tests` target built from the
global tree with `-j4` in 2m09.500s (peak RSS 1,688,652 KiB; no swap). The four-case
`Spec189RepoPublication` selector passed, including root/index loading, selected node and
shared dependency reads, bounded materialization, non-canonical order rejection and
corrupted-object rejection. This is a Repo-side consumer seam only; protected encrypted
serving, CollaborationContext/Provider wiring, dynamic cancellation injection and real
Qwen preparation remain unobserved. Raw logs are
`.codex-tmp/spec189-b189-1b-consumer-build-r1.log` and
`.codex-tmp/spec189-b189-1b-consumer-selector-r1.log`.

2026-09-19 Spec189 ONNX identity/resource retry r30-r31: the strict native
assembly path was changed to parse and inline the canonical source once, then
derive graph/initializer identity from that owned model. The read-only review
and focused ONNX selectors passed. The maintained Qwen launcher still stopped
at `RESOURCE_BOUNDARY:swapIo` before an interpretable two-provider workload:
r30 reached MiniNDN/model preparation but not workload, while r31 stopped
before MiniNDN startup after a new candidate build. Cleanup completed and no
provider execution, terminal output, or qualification result was observed.
The raw records remain under
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r30/`
and `two-provider-global-r31/`; see
`specs/189-qwen-two-provider-minindn/evidence/b189-onnx-identity-resource-20260919.md`.

2026-09-19 Spec189 Qwen r32 resource retry: an initial prepare invocation using
the stale example profile was rejected at `NATIVE_BUILD_RECEIPT_PREFLIGHT` and
did not create a run record. The corrected global profile then prepared
successfully, but the run stopped after 13 samples at
`RESOURCE_BOUNDARY:swapIo` before MiniNDN startup; cleanup was `PASS`, and
MiniNDN/workload were `NOT_EVALUATED`. The run record is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r32/`.
This is a host resource boundary, not a Qwen protocol result.

2026-09-19 Spec189 Qwen r33 resource retry: after reducing the host swap
configuration, prepare succeeded and the runner entered its sampling phases,
but it still stopped before MiniNDN startup at `RESOURCE_BOUNDARY:swapIo`.
Twenty-one samples reached about 275 MB swap-I/O delta with only about 310 MB
RSS; cleanup was `PASS`, and MiniNDN/workload remained `NOT_EVALUATED`.
Repeated runs with the same host paging baseline are therefore stopped rather
than classified as native or protocol failures. Raw evidence is under
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r33/`.

2026-09-19 Spec189 focused selector invocation boundary: running the existing
`unit-tests --run_test=Spec189*` binary from inside the build directory caused
`Spec189RepoPublication/MaterialManifestPublishesWithOwnedTransactionsAndRejectsCorruption`
to fail its relative fixture open (`tests/fixtures/.../extraction-vectors.json`).
No production assertion failed. Re-running the same selector from the repository
root passed all 4 cases; the build-directory invocation is not accepted as test
evidence.

2026-09-19 Spec189 Qwen r35 resource boundary: with swap disabled and the
correct frozen global-r3 candidate, the maintained runner entered real MiniNDN;
Controller, Authority and both Providers became ready. During the requester
preparation/model-loading phase the host guard observed
`RESOURCE_BOUNDARY:MemAvailable`: the minimum available memory was
`1453481984` bytes against the `1610612736`-byte floor, while swap-I/O delta
remained zero. The largest process in the run group reached
`4217356288` bytes RSS, cleanup was `PASS`, and no workload record or protocol
verdict was produced. This is a host resource boundary, not a protocol result;
the durable evidence is [Spec189 r35 resource boundary](../specs/189-qwen-two-provider-minindn/evidence/b189-resource-r35-20260919.md)
and the raw trace remains under
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r35/`.

2026-09-19 Spec189 Qwen r36 resource boundary: the direct-vector source reader and
selective ONNX identity/shape materialization had been built into the global
candidate. The maintained runner again reached MiniNDN, Controller, Authority and
both Provider readiness, then stopped during model preparation at
`RESOURCE_BOUNDARY:MemAvailable` with a minimum of `1557188608` bytes against the
`1610612736`-byte floor. Swap was disabled and both swap-used and swap-I/O deltas
were zero; the largest process reached `4591411200` bytes RSS. Cleanup drained all
processes, but no requester result, workload record, or qualification verdict was
produced. This is a host resource boundary rather than a protocol failure or PASS.
See [Spec189 ONNX/r36 evidence](../specs/189-qwen-two-provider-minindn/evidence/b189-onnx-memory-r36-20260919.md)
and raw samples under
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r36/`.

2026-09-19 Spec189 protected Runtime selector r1-r3: the new C++ selector reached the
production Runtime default publisher and committed protected Repo envelopes, but the
first attempt stopped at a root-owned `/tmp/ndnsf-large-data` staging directory
(`Permission denied`). After switching to a fixture-owned spool, the next attempt
stopped at the test oracle's use of `RepoCore::get()`, which the configured filesystem
backend intentionally rejects above its vector compatibility threshold with
`repo-large-object-vector-path-disabled`. No production assertion or protocol result
failed. The raw logs are
`.codex-tmp/spec189-b189-2-selectors-20260919/protected-runtime-r1.log`,
`protected-runtime-r2.log` and `protected-runtime-r3.log`; the selector is being
repaired to use bounded `getRange()` reads before the next build.

2026-09-19 Spec189 protected Runtime selector retry: after the fixture-owned spool and
bounded `getRange()` oracle repairs passed the read-only review, the global-r3
`spec189-prepared-request` target rebuilt in 32.795s with `-j1`; the two C++ Spec189
cases then passed from the repository root. The result closes only the local protected
publication selector boundary. ACK/Selection, Provider assembly, real Qwen execution,
output oracle, resource drain and MiniNDN qualification remain unobserved.
Raw logs are `.codex-tmp/spec189-b189-2-selectors-20260919/prepared-request-build-r5.log`
and `.codex-tmp/spec189-b189-2-selectors-20260919/protected-runtime-batch-r1.log`.

2026-09-19 Spec189 B189-1b protected material receipt retry: the selector was extended
after read-only review to bind root material-manifest name/digest and every payload
id/name/digest to the publication receipt, then perform a non-empty, at-most-1024-byte
`RepoCore::getRange()` read for the material manifest and each payload. The affected
`spec189-prepared-request` target rebuilt from global-r3 with `-j1` in 29.085s and both
`Spec189*` C++ cases passed from the repository root. This closes the local protected
atomic-material publication/readability boundary only; decrypted material, ACK/Selection,
Provider assembly, real Qwen execution, output, drain and MiniNDN qualification remain
unobserved. Logs: `.codex-tmp/spec189-b189-2-selectors-20260919/prepared-request-build-r6.log`,
`protected-runtime-material-r1.log`, and `protected-runtime-material-batch-r1.log`.

2026-09-19 Spec189 material-consumer selector r1: the DI integration target
built successfully, but invoking `Spec189MaterialConsumerBoundsSelectedPayloadFetches`
from `build-spec189-b189-3-global-r3/` stopped in the test fixture preflight because
the worker locator did not include the current build root. No production assembler
or protocol assertion ran. Raw output and the exact environment are preserved under
`.codex-tmp/spec189-b189-material-selector-r1/`; the retry must set
`NDNSF_SPEC182_BIN_DIR` to the configured build root.

2026-09-19 Spec189 material-consumer full assembly suite r1: the new selector
passed, but the suite invoked from the build directory stopped first in the
existing `CollaborationContextBindsAssignmentRootBeforeSourceFetch` fixture when
NDN-CXX could not open relative `examples/trust-any.conf`. The new material
consumer and the remaining seven assembly cases reached completion in that run;
the working-directory failure is not a production assertion. Raw output is
`.codex-tmp/spec189-b189-material-selector-r2/full-suite.log`; retry from the
repository root with the configured build root in `NDNSF_SPEC182_BIN_DIR`.

2026-09-19 Spec189 material-consumer unit build r1: `unit-tests -j1` reached the
new `spec189-repo-publication.t.cpp` translation unit and failed because the Repo
metadata assignment referred to a `materialManifestBytes` local outside its scope.
No unit executable was linked and no assertion ran. The exact compiler output is
`.codex-tmp/spec189-b189-material-unit-build-r1/build.log`; after the one-line
scope repair, the affected unit target built and `Spec189*` passed 4/4 from the
repository root.

2026-09-19 Spec189 requester Repo integration r1: the active incremental build was
intentionally interrupted (rc=68) after additional static review found two Boost
1.71 vector-printability violations in the new assertions and an avoidable initializer
copy in the recovery branch. These were not compiler/runtime observations. Preserve
`.codex-tmp/spec189-qwen-repo-requester-build-r1/build.log`; r5 changes the assertions
to collection comparisons and moves the initializer. Changed gate and review identity:
`specs/189-qwen-two-provider-minindn/evidence/b189-requester-repo-20260919.md`.
Do not reuse the withdrawn r4 STATIC_PASS for validation.

2026-09-19 Spec189 requester Repo r2 build FAIL (rc=1): DI_NativeRequester.cpp
calls makeFilesystemRepoStore without including FilesystemRepoStoreBackend.hpp.
This is a declaration/include failure, before requester linkage or runtime.
Preserve `.codex-tmp/spec189-qwen-repo-requester-build-r2/build.log`.
Changed gate: inspect factory declaration -> definition translation unit -> Repo
library -> requester target use; see `evidence/b189-requester-repo-20260919.md`
in Spec189. No MiniNDN run occurred.

2026-09-19 Spec189 requester Repo runtime selector r1: corrected build passed and
Spec189RepoPublication passed 5/5. PrepareSuccessUsesTheProductionRuntimeEntry
then stopped at DI_NATIVE_ENCRYPTED_PUBLICATION_FAILED: cannot create large-data
staging file: Permission denied (rc=201). Preserve
`.codex-tmp/spec189-qwen-repo-requester-build-r3/runtime-test.log`.
Changed gate for fixture retry: explicitly select a private run-owned
NDNSF_REQUEST_LARGE_DATA_DIR, verify its permissions, retain the same binary/source.
This is not a remote Provider or MiniNDN protocol result.

2026-09-19 Spec189 B189-1c external-initializer fast path: the frozen
`NativeOnnxRecipeAssembler.cpp` range-digest change received official read-only
`STATIC_PASS`; the unit target and affected DI production targets rebuilt with
system-first Waf `-j4`, and `Spec182OnnxIdentity` passed 13/13. The maintained
root MiniNDN run `two-provider-global-r44` reached Controller, Authority, both
Providers, signed ACK offers and `GRANT_VERIFICATION` before the first boundary
changed to `NATIVE_STREAM_FAILED` / `stream event gap exceeded retry budget`.
The supervisor drained all children (`cleanup=PASS`); peak aggregate RSS was
`3619823616` bytes, minimum available memory `3538112512` bytes, and swap-I/O
delta was zero. No terminal response, runner completion or qualification was
observed. Raw run data remains under
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r44/`;
the detailed checkpoint is
`specs/189-qwen-two-provider-minindn/evidence/b189-onnx-fastpath-20260919.md`.
The former r39 preparation-timeout boundary is reduced but not resolved into a
product PASS; the next retry must diagnose the post-grant stream gap.
