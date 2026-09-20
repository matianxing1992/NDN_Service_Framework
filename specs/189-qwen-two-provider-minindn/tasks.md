# Tasks: Qwen 0.6B Two-Provider MiniNDN Full-Path Validation

**Configure dependency checkpoint**: 2026-09-19 — 新增根 `configure.sh`（缺失 OS 包安装后调用 Waf，NDN 库不安装）和依赖清单汇总，Waf 补 SQLite/OpenSSL/Protobuf/ONNX/ORT 编译链接探针。5 项隔离测试、本机只读 inventory、Bash/ShellCheck、Waf Python 语法及限定 diff 检查通过。见 [边界与用法](../../docs/configure-dependencies.md)。未执行 apt、完整 Waf configure/build 或模型实验；专用 SDK 自动安装仍未完成，产品任务不关闭。既有混合暂存区和 hook 阻塞未解除，变更留在工作树，不混合提交或 push。下一步固定专用 SDK 来源/摘要及安装契约，再做独立 configure 验收。

**Repo-handle design checkpoint**: 2026-09-19 — 用户接受 [高层 R6](../../Design/highlevel-design.md)：不可变材料引用与可更新副本 locator 分开，存储返回 handle，读取保留原名并使用经验证的 hint；DI 保持授权/Selection/placement 约束及来源生命周期。文档链接和限定 diff 检查通过；仅高层目标，未修改源码、运行实验或关闭任务。混合暂存区与既有 hook 阻塞未解除，未混合提交。下一步细化引用契约和真实转发验收。

**Repo-mode design checkpoint**: 2026-09-19 — 用户接受两模式约束，更新 [高层 Repo/DI 设计](../../Design/highlevel-design.md) 与 [设计变更记录](../../Design/spec-design-changes.md)。in-app 不接收外部写入、不承担副本责任，但 DI 可主动读取所分配材料并本地缓存；长期保存显式提交 server，活跃读取由 owner/lease 保护，KV 不自动复制。文档链接和限定 diff 检查通过；无源码/API/实验变化，不关闭产品任务。现有混合暂存区及此前提交 hook 阻塞未解除，本轮不混合提交；下一步细化模式准入与 DI 生命周期验收。

**SIF design checkpoint**: 2026-09-19 — 按用户要求补充 [S1–S3](../../Design/highlevel-design.md#sif-构建与复用约束)：先验证并封存全依赖/SDK base，容器内 Waf 构建 NDNSF 并生成新完整 SIF；依赖身份不变时复用 base，改变时增建/重建并重新验证。已对照 TigerCluster 两层文档与 iTiger 维护技能；高层文档 19 个链接均可解析，S1–S3 及限定 diff 检查通过。仅文档更新，未启动 SIF/MiniNDN/集群运行，不改变当前产品验收状态。索引已有并行源码修改且此前提交钩子阻塞，本轮未混合提交。后续交付按 S1–S3 核对输入与镜像身份。

**Installed-runtime policy checkpoint**: 2026-09-19 — 用户新增 [高层设计 B/M 约束](../../Design/highlevel-design.md#编译安装与-minindn-实验约束)：后续 MiniNDN 必须使用系统安装的库、应用、worker 和被测模块；构建树 selector 仅作构建阶段测试。下一次 MiniNDN 前须核对启动器与实际加载路径，存在构建树依赖时先迁移安装规则。本轮仅文档更新，未构建/安装/运行，未关闭产品任务；历史证据保持原样。

**High-level design checkpoint**: 2026-09-19 — 新增 [四模块高层设计](../../Design/highlevel-design.md)，各模块 500–1000 字，包含用例、当前边界和长期原则；README、设计管理与架构阅读入口已接入原则检查。字数、链接与限定 diff 检查通过。仅文档治理，不修改产品代码、API、冻结目标或实验状态，不关闭任何原任务；下一次实施先映射适用原则再验证生产路径。本地 checkpoint 提交被现有索引全量检查拦截（`development-assistant files or references remain in the Git index`），七文件本轮变更保持精确暂存，未绕过 hook、未 push；其他并行改动未纳入。

**Installer maintenance checkpoint**: 顶层安装入口已修正；12 项隔离测试、ShellCheck、Bash 语法与本机只读依赖检查通过。未运行完整安装或原生构建，不改变本 Spec 的 PARTIAL 状态。见 [installer audit](../../docs/install-stack-audit.md)。

**Status**: IN_PROGRESS
**Input**: [spec.md](spec.md), [plan.md](plan.md), [batch-execution.md](batch-execution.md)
**Rule**: Production C++ → C++ assertions → Python orchestration。每任务编码、fixture/调用方/构建注册完成后冻结静态审查；同批组合通过再统一增量构建测试。静态通过不是完成。

## Current Checkpoint

**B189-3 r139 authenticated-Selection disk-free boundary**: 2026-09-20 — r139
used a fresh run/external root, the repaired launcher, the exact installed
candidate, unchanged resource/stream limits, and `NDNSF_NDN_LOG='*=TRACE'`.
The unchanged host guard stopped at `RESOURCE_BOUNDARY:diskFree`; cleanup
passed. The 315-sample record reached minimum available memory
`2534084608`, minimum disk free `4123873280`, aggregate RSS peak
`8117018624`, owned-swap peak `156700672`, and swap-I/O delta `458199040`.
The requester emitted `NDNSF_DI_NATIVE_ACK_CLOSED` and
`NDNSF_DI_NATIVE_SELECTION_COMMITTED`; both Providers accepted authenticated
Selection and recorded `GRANT_VERIFICATION` at `BEFORE_ASSEMBLY`. Provider-0
entered placement-bound dependency preparation, reported assembly admission,
and emitted `ASSEMBLY_STARTED`; both Providers emitted `EXECUTION_ENTERED`.
The requester received signed `SELECTION_STATUS_QUERY` replies from both
Providers. Provider-0 retained a `752097499`-byte staging `canonical.onnx`
and a `752308868`-byte protected assembly cipher. No `RUNNER_READY`,
`EXECUTION_COMPLETED`, terminal response, second request, or C++ oracle result
exists. The request cancellation is a consequence of the guard stop, not a
protocol rejection. Raw output is `.codex-tmp/spec189-r139-launch.log`; raw
run root is `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r139/`.
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
bounded static/runtime review of the r139 materialization and resource-owner
boundary, followed by a fresh guarded run without changing limits or deleting
raw evidence.

After r139, its preserved Provider-0 staging `canonical.onnx` was independently
verified as digest
`sha256:258f367628b69a66eccf0973386b2eb80e2fce74cd64c2172d4f00a6e06abb94`
and atomically hard-linked to canonical staging inode `4853426`; the r139 path
and bytes remain, the inode has `11` links, and root ext4 free space is
`4875534336` bytes. The unique protected ciphertext and raw logs were not
changed. This is space-preserving evidence maintenance only; no task status
changes.

The first read-only re-review of the r139 Changed gate used immutable snapshot
`.codex-tmp/spec189-r139-source-staging-review-v1/` and returned
`NOT_STATIC_PASS`: the snapshot omitted `NativeModelRunner.hpp`, the protected
cleanup catch could mask the first assembly error, and the artifact/cache owner
handoff plus cleanup-failure evidence needed repair. No rebuild or rerun was
performed. The bounded repair preserves the first exception, transfers the
protected artifact-directory lifetime through `NativeModelRunnerSpec` into the
cache cleanup callback, and emits
`NDNSF_DI_PROVIDER_ARTIFACT_CLEANUP_FAILED`; a corrected immutable snapshot
and read-only review are required before build.

The second read-only re-review used immutable snapshot
`.codex-tmp/spec189-r139-source-staging-review-v2/` and found one P1: the
protected success path used `std::filesystem::remove(sourceFile)`, bypassing
the pinned-directory fd's overwrite/fsync/unlink cleanup. No build or rerun was
performed. The repair now returns a file eraser bound to the registered
directory lease fd, reuses the same secure entry cleanup for early
`canonical.onnx` release, rejects non-direct children, and preserves the
stable source-staging failure marker. A new immutable snapshot and read-only
review are required before the affected build.

The third read-only re-review used immutable snapshot
`.codex-tmp/spec189-r139-source-staging-review-v3/` and found a P1 race between
the returned early eraser and the final runtime lease drain, plus a P2 in the
range-backed template/node validation path that passed `data()` (which is null
for a range source) to protobuf. No build or rerun was performed. The repair
serializes early erase and final drain with one directory-lease mutex and
materializes validation views through `copyBytes()`. A new immutable snapshot
and read-only review are required before the affected build.

The fourth immutable snapshot
`.codex-tmp/spec189-r139-source-staging-review-v4/` passed the read-only review
with `STATIC_PASS`: all 20 current-file hashes and the diff hash matched
`base-head` `78e1a4ca`, and no actionable P0-P3 finding remained. The review did
not build, install, run MiniNDN, or qualify Spec189. The affected C++ build and
focused selectors are now the next gate.

**Evidence maintenance after push**: 2026-09-20 — after checkpoint
`d13d6045` was pushed to `origin/Experimental`, 104 old run-scoped
`encrypted-repo`, `canonical-repo`, and Provider cache directories were removed
from the local `.codex-tmp` workspace. The removed payload/cache set was about
`41 GiB`; old run logs, JSON, certificates, and resource samples were retained,
and the complete r139 run root was retained at
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r139/`.
The remaining Spec189 runs directory is about `4.9 GiB`, and root free space
rose from about `4.1 GiB` to `42 GiB`. This is local evidence maintenance only:
no protocol result or task checkbox changes, T005/T006/T007/T009 remain
`PARTIAL`, and no Codex conversation files or Git refs were modified.
Details are in [run artifact cleanup evidence](evidence/b189-run-artifact-cleanup-20260920.md).

**B189-3 r119 range-source focused-check boundary**: 2026-09-20 — the first
focused selector invocation after the bounded publication repair used the
wrong worker fixture directory (`/usr/local/bin` instead of
`build-spec189-oracle`), so the material-consumer and ONNX activation cases
stopped before their assertion bodies. The Repo publication case did execute
and found a caller mismatch: external initializer range-view payloads were
still measured and written through `MaterialPayload::bytes.size()`, producing
`repo-publication-identity-mismatch`. The repair now adds a verified Repo
range-backed source, bounded `copyBytes()` publication, and `byteSize()` Repo
metadata checks. Raw selector logs are
`.codex-tmp/spec189-r119-material-selector.log` and
`.codex-tmp/spec189-r119-onnx-repo-selector.log`; durable detail is in
[r119 focused-check evidence](evidence/b189-r119-range-source-focused-check-20260920.md).
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
to rebuild the affected targets, rerun with the explicit build fixture
directory, and review the resulting selectors before installation.

The follow-up rebuild passed `434/434`; with the correct fixture directory the
material consumer selector passed `2/2` and the ONNX selectors entered their
assertion bodies. The Repo publication selector then exposed two further
source gaps: strict `getRangeIfCurrent()` identity did not match the persisted
manifest representation, and external initializer selection fetched only the
header instead of all authenticated `chunkPayloadIds`. No checkbox changes;
T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r119-material-selector-v2.log` and
`.codex-tmp/spec189-r119-onnx-repo-selector-v2.log`; the same durable r119
evidence records this follow-up. Next step is to repair these two C++ Repo
selection boundaries, rebuild, and rerun the same selectors.

That rerun passed the material consumer `2/2`, the ONNX extraction/assembly/
activation selectors, and the Repo source-range check, but the Repo
post-Selection case stopped at `selected material payload differs from
manifest reference`: chunk IDs are authenticated by a shared-initializer
reference, while their individual digest/size are in the root's authenticated
`materialObjects` record. No checkbox changes; T005/T006/T007/T009 remain
`PARTIAL`. Next step is to bind chunk selection to its owning authenticated
reference, rebuild, and rerun the same C++ selectors.

The r119 repair then passed the affected native build `434/434`. The material
consumer selector passed `2/2`; the combined ONNX extraction, assembly,
activation, and Repo publication selectors completed with `*** No errors
detected`, including bounded Repo range reads and post-Selection chunk
binding. These are focused C++ checks only: no task checkbox changes;
T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r119-material-selector-v4.log`,
`.codex-tmp/spec189-r119-onnx-repo-selector-v4.log`, and
`.codex-tmp/spec189-r119-build-range-source-v4.log`. Next step is static diff
review, exact installed-runtime rebuild/installation, loaded-path/hash
verification, and a fresh guarded MiniNDN request-chain run.

The first fresh installed-runtime attempt was stopped before MiniNDN at a
launcher invocation boundary: `/usr/bin/python3` under `sudo env` could not
import the user-installed `ndn` package, so provider key-prefix decoding was
skipped even though the preserved PIB contained the provider key. No checkbox
changes; T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r120-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r120/`.
Next step is a fresh run root with the same candidate and explicit digests,
plus the verified user-site `PYTHONPATH`.

The r121 retry used that explicit `PYTHONPATH` and the installed candidate. It
passed launcher key initialization, MiniNDN startup, both Provider readiness/
offer decisions, and both `NDNSF_DI_GRANT_VERIFICATION` checks at
`BEFORE_ASSEMBLY`, then the unchanged host guard stopped at
`RESOURCE_BOUNDARY:MemAvailable` before requester ACK/Selection. Across 298
samples, available memory reached a minimum of `1003737088` bytes, owned swap
reached `524922880` bytes, swap-I/O delta reached `1107390464` bytes, and
aggregate RSS reached `9811668992` bytes. The assembly worker child reached
approximately `5112135680` bytes RSS while its provider parent was
approximately `2050000000` bytes RSS. Cleanup passed. The requester recorded
`CANCELLED`; no ACK_CLOSED/Selection, runner, execution, terminal, repeat
round, or oracle result exists. No checkbox changes; T005/T006/T007/T009
remain `PARTIAL`. Raw output is `.codex-tmp/spec189-r121-launch.log`, raw run
root is `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r121/`,
and durable detail is in [r121 resource evidence](evidence/b189-r119-range-source-focused-check-20260920.md).
The next step is a bounded C++ worker ownership repair: release parent-side
model/initializer buffers only after the request pipe is fully written, then
rebuild, rerun focused selectors, reinstall, and use a fresh guarded run root.

The r122 repair adds an optional `sourceToReleaseAfterWrite` argument to the
OA02 worker transport. Existing four-argument callers remain non-destructive;
production `NativeCanonicalOnnxAssembler` opts in and releases the parent
model/initializer/material source only after the complete request frame enters
the pipe. The affected targeted build completed `538/538`; the material
consumer selector passed `2/2`, and the combined ONNX extraction, native
assembly, activation, and Repo publication selector passed all `30` cases.
The installed five-target candidate has exact build/installed SHA-256 matches;
the corrected `ldd`/RPATH check found no unresolved or build-tree dependency.
No checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Raw outputs are
`.codex-tmp/spec189-r122-targeted-build.log`,
`.codex-tmp/spec189-r122-material-selector.log`,
`.codex-tmp/spec189-r122-onnx-repo-selector.log`, and
`.codex-tmp/spec189-r122-runtime-identity-v2.log`. The next step is the new
guarded MiniNDN run root `two-provider-global-r122`.

The fresh r122 MiniNDN run entered with the repaired installed candidate and
both Providers reached `NDNSF_DI_NATIVE_PROVIDER_READY`. Before requester
`ACK_DECISION` or Selection, the unchanged host guard stopped at
`RESOURCE_BOUNDARY:diskFree`: across 45 samples disk free reached a minimum of
`4001157120` bytes against `4294967296`; available memory stayed at or above
`6973997056` bytes, aggregate RSS peaked at `3527479296` bytes, owned swap at
`4096` bytes, and swap-I/O delta at `180342784` bytes. Cleanup passed and no
process remained. No requester ACK/Selection, grant verification, assembly,
runner, execution, terminal, repeat round, or oracle result exists. No
checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r122-launch.log`, raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r122/`,
and durable detail is in [r122 disk evidence](evidence/b189-r119-range-source-focused-check-20260920.md).
The next step is a bounded disk-artifact/working-set review and a
space-preserving run setup; do not raise the guard or delete preserved failure
evidence implicitly.

The disk review found the root filesystem at 98% use and the old r118/r121
Repo payload copies were byte-identical to the canonical initializer. Their
existing paths were retained and hard-linked to the canonical immutable inode,
releasing approximately `2.2G`; the r122 incomplete staging `.part` digest did
not match and was left untouched. Root free space then measured
`7007141888` bytes. No code, installed candidate, guard limit, or durable raw
run was deleted. No checkbox changes; T005/T006/T007/T009 remain `PARTIAL`.
The next step is a fresh r123 guarded MiniNDN run using the same verified
candidate and a new run root, after recording this space-preserving boundary.

The first r123 invocation stopped before MiniNDN because the oracle digest
argument omitted a `4`; preflight returned
`SPEC189_ORACLE_BINARY_DIGEST_MISMATCH`. The verified build/installed digest
is `sha256:ef12b58e7fb2e3b0f0643ab7afafe02a3786456150c178a00402a3adef7b9086`.
No checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r123-launch.log`. The next step is a fresh invocation
with only this digest corrected.

The corrected r124 invocation entered MiniNDN with the same installed
candidate, but before requester `ACK_DECISION`/Selection the unchanged guard
stopped at `RESOURCE_BOUNDARY:diskFree`; cleanup passed and no process
remained. The r124 run root accumulated approximately `4.0G`, dominated by
the requester encrypted Repo payload set; available memory stayed healthy.
No grant verification, assembly, layer fetch, runner, execution, terminal,
repeat round, or oracle result exists. No checkbox changes;
T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r124-launch.log`, raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r124/`,
and durable detail is in [r124 disk evidence](evidence/b189-r119-range-source-focused-check-20260920.md).
The next step is the bounded experiment-runner storage-path repair; it must
keep the guard and production ownership contract unchanged.

The r125 runner repair adds optional `--encrypted-repository-path` placement;
the default remains under the run root, while an explicit path must be a new
or empty directory outside that root. Focused Python regression and guard
tests passed `44` cases, and `py_compile` passed. No checkbox changes;
T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r125-runner-path-tests.log`. The next step is a fresh
guarded MiniNDN run with the external ciphertext path and the same installed
native candidate.

The fresh r125 run used `/dev/shm/ndnsf-spec189-r125/encrypted-repo` and
therefore passed the prior root-disk boundary: both Providers reached READY,
signed `ACK_DECISION`, and `NDNSF_DI_GRANT_VERIFICATION` at
`BEFORE_ASSEMBLY`; Provider-0 also entered assembly staging and fetched the
canonical ONNX. The unchanged host guard then stopped at
`RESOURCE_BOUNDARY:ownedSwap` after 284 samples: minimum available memory was
`2282303488`, minimum root disk free was `5001625600`, aggregate RSS peaked at
`6933381120`, and owned swap peaked at `302174208` against the unchanged
`268435456` limit. Cleanup passed and no process remained. There is no
`RUNNER_READY`, execution, terminal response, second request, or oracle result;
T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r125-launch.log`, raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r125/`,
and durable detail is in [r125 resource evidence](evidence/b189-r119-range-source-focused-check-20260920.md).
The tmpfs ciphertext placement is not a qualification result; after preserving
the run, all 24 validated canonical payload paths were hard-linked to the
immutable source, increasing ext4 free space to `20034101248` bytes. The next
step is a fresh r126 run using ext4-backed run-scoped ciphertext and the same
guard/candidate.

The first r130 invocation stopped before MiniNDN at
`ASSEMBLY_WORKER_BINARY_DIGEST_MISMATCH` because the manually supplied worker
hash had a character transposition. No checkbox changes and no protocol or
resource result exists. The verified build/installed worker digest is
`sha256:1a97d902e0c9a2e5d8acbaf34046a9a92ae6a77d9d494a27671b2deb75b3f4b6`.
Raw output is `.codex-tmp/spec189-r130-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r130/`.
The next step is r131 with hashes read directly from the verified files.

r126 used the ext4-backed external ciphertext directory and crossed the r125
tmpfs/swap boundary. Both Providers reached READY, signed `ACK_DECISION`, and
`NDNSF_DI_GRANT_VERIFICATION` at `BEFORE_ASSEMBLY`; Provider-0 fetched
`canonical.onnx` into assembly staging. The unchanged host guard then stopped
at `RESOURCE_BOUNDARY:MemAvailable` before `RUNNER_READY`, execution, or
terminal response. Across 299 samples, available memory reached a minimum of
`1174708224`, disk free a minimum of `16242237440`, aggregate RSS a maximum of
`8260157440`, owned swap a maximum of `105701376`, and swap-I/O delta a maximum
of `1005477888`. Cleanup passed and no process remained. No checkbox changes;
T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r126-launch.log`, raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r126/`,
and durable detail is in [r126 resource evidence](evidence/b189-r119-range-source-focused-check-20260920.md).
The next bounded change is a finite MiniNDN NFD Content Store size; guard
limits, native ownership, and production Repo/DI contracts remain unchanged.

The MiniNDN-only repair sets `MININDN_NFD_CS_SIZE=4096` instead of the
MiniNDN default 65536 entries, so forwarders do not each retain multi-GiB
canonical/large-data working sets. The authenticated Repo remains the source
of truth and the production NFD default, guard, and native ownership contracts
are unchanged. The focused Python and guard regression set passed `44` cases;
runner `py_compile` and `git diff --check` passed. No checkbox changes;
T005/T006/T007/T009 remain `PARTIAL`. The next step is a fresh guarded r127
MiniNDN run with ext4-backed encrypted Repo storage and the same installed
candidate.

r127 使用 `MININDN_NFD_CS_SIZE=4096` 后仍在
`RESOURCE_BOUNDARY:MemAvailable` 停止。两 Provider 到达 READY、ACK offer
和 `GRANT_VERIFICATION=BEFORE_ASSEMBLY`，但 worker RSS 峰值约
`6283599872`，aggregate RSS 峰值 `8255197184`；available memory 最低
`1493688320`，owned swap 最大 `238215168`，cleanup 通过。低 CS 降低了
NFD 常驻，却放大了 fetch worker 工作集，因此不计资源/协议 PASS；没有
`RUNNER_READY`、execution、terminal、第二请求或 oracle。T005/T006/T007/T009
仍为 `PARTIAL`。Raw output 为 `.codex-tmp/spec189-r127-launch.log`，raw
run root 为 `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r127/`。
下一步是保留 guard 不变，测试中间 NFD CS 容量。

The next bounded candidate sets `MININDN_NFD_CS_SIZE=32768`; this is a
MiniNDN-only cache parameter change and does not alter production NFD defaults,
Repo/DI ownership, or host guard limits. The required focused runner tests and
`py_compile` gate must pass before the fresh r128 run.

The first r128 invocation stopped in preflight at
`MODEL_TOKENIZER_DIGEST_MISMATCH` before MiniNDN; a direct hash check confirms
the stage-manifest tokenizer is
`sha256:aeb13307a71acd8fe81861d94ad54ab689df773318809eed3cbe794b4492dae4`.
This is an invocation boundary with no task checkbox change and no protocol or
resource result. Raw output is `.codex-tmp/spec189-r128-launch.log`; the fresh
r128 run root is `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r128/`.
The next step is r129 with the exact verified tokenizer digest.

r129 passed tokenizer preflight and used ext4-backed ciphertext with
`MININDN_NFD_CS_SIZE=32768`. Both Providers reached READY, ACK offer, and
`GRANT_VERIFICATION=BEFORE_ASSEMBLY`; Provider-0 fetched canonical ONNX, then
the unchanged guard stopped at `RESOURCE_BOUNDARY:MemAvailable`. Across 298
samples, available memory reached `1170264064`, disk free `8675835904`,
aggregate RSS `8783286272`, owned swap `105500672`, and swap-I/O delta
`740933632`; cleanup passed. The assembly worker peaked at about
`5636308992` RSS, so this is a native worker working-set boundary, not a
protocol or qualification result. No `RUNNER_READY`, execution, terminal,
second request, or oracle exists; T005/T006/T007/T009 remain `PARTIAL`.
Raw output is `.codex-tmp/spec189-r129-launch.log`, raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r129/`.
The next bounded change is the C++ certified-chain move/ownership repair; no
guard or NFD production default change is authorized by this evidence.

The r130 C++ repair moves the authenticated original protobuf into S5 shape
inference and retains only compact certified node bytes for S6 comparison;
the certified checks and protocol contracts are unchanged. Targeted build
`538/538`, material selector `2/2`, and ONNX/Repo selector `30` cases passed.
Requester, Provider, worker, oracle, and DI library build/installed hashes
match, and the installed closure has no unresolved or build-tree dependency.
No checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Raw outputs are
`.codex-tmp/spec189-r130-targeted-build-move-original.log`,
`.codex-tmp/spec189-r130-material-selector-move-original.log`,
`.codex-tmp/spec189-r130-onnx-repo-selector-move-original.log`, and
`.codex-tmp/spec189-r130-runtime-identity.log`. The next step is a fresh
guarded MiniNDN run using the repaired installed candidate.

The r131 fresh installed-runtime run used exact hashes derived from the
verified runtime files and the r130 certified-chain move repair. It passed
MiniNDN startup, both Provider READY/ACK offers, and both
`GRANT_VERIFICATION=BEFORE_ASSEMBLY` checks, but the unchanged host guard
stopped at `RESOURCE_BOUNDARY:ownedSwap` before requester ACK/Selection. The
303-sample stream recorded minimum available memory `1645875200`, minimum disk
free `4884680704`, maximum aggregate RSS `8443310080`, maximum owned swap
`405467136`, and maximum swap-I/O delta `968613888`; cleanup passed. The
requester only recorded `CANCELLED`; there is no Selection, runner, execution,
terminal, repeat request, or oracle result. No checkbox changes;
T005/T006/T007/T009 remain `PARTIAL`. Raw output is
`.codex-tmp/spec189-r131-launch.log`, raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r131/`.
The next bounded C++ change releases and scrubs the worker's consumed source
buffers immediately after `ownedSourceModel()` returns, before S4-S7 continue.

The first r132 targeted build stopped in compilation because the new release
point called `scrub()` on `NativeCanonicalByteBuffer`, while that helper exists
only on `MaterialPayload`. No binary or selector ran; the raw compiler output
is `.codex-tmp/spec189-r132-targeted-build-source-release.log`. The bounded
repair uses `OPENSSL_cleanse` on `initializerBytes->asVector()` before reset;
the next step is the same targeted build retry. Task checkboxes remain
unchanged.

The first r132 focused-selector command then used the non-existent filter
`Spec189CanonicalMaterialConsumer/*` and stopped at Boost.Test setup with
"no test cases matching filter"; the second command entered all `30` named
ONNX/Repo cases and passed. This is a selector invocation boundary, not a
source assertion result. Raw outputs are
`.codex-tmp/spec189-r132-material-selector-source-release.log` and
`.codex-tmp/spec189-r132-onnx-repo-selector-source-release.log`; enumerate the
actual built test tree and rerun the material consumer with the exact filter
before installation.

The exact r132 material selector then entered both cases but failed
`ExternalInitializerUsesBoundedChunksAndReassemblesAfterSelection`: payload
cleanup zeroed a shared initializer backing that was still owned by the source,
so the post-Selection round-trip comparison saw different bytes. The ONNX/Repo
selector remained `30`-case PASS. This is a C++ ownership assertion boundary,
not a MiniNDN result. The bounded repair makes `MaterialPayload::scrub()` a
no-op for shared `backing`/range views and clears only payload-owned bytes;
rebuild the affected targets and rerun the exact material selector.

r132c passed the affected build `538/538`, the exact material selector `2/2`,
and the ONNX extraction/assembly/activation/Repo selector `30/30`. The
source-release repair is therefore focused-tested, but it does not close
T003/T005/T006/T007/T009 or establish a protocol result. Raw outputs are
`.codex-tmp/spec189-r132c-targeted-build-shared-scrub.log`,
`.codex-tmp/spec189-r132c-material-selector-shared-scrub.log`, and
`.codex-tmp/spec189-r132c-onnx-repo-selector-shared-scrub.log`. The next step
is exact installation of the affected DI/Provider/worker/requester/oracle
candidate and loaded-path identity verification before a fresh guarded run.

r132c installed the affected DI library, Provider, assembly worker, requester,
and C++ oracle successfully. Each build/install pair has exact SHA-256
equality; the verified installed `ldd` closure contains no unresolved,
build-tree, or `.codex-tmp` dependency. Evidence is in
`.codex-tmp/spec189-r132c-runtime-identity-v3.log` and
`.codex-tmp/spec189-r132c-ldd-closure.log`. This is an installed-runtime
identity gate only; T005/T006/T007/T009 remain `PARTIAL`. The next step is the
fresh guarded MiniNDN run root `two-provider-global-r132`.

r132 entered MiniNDN and both Providers reached READY, but the unchanged host
guard stopped at `RESOURCE_BOUNDARY:diskFree` before requester ACK/Selection.
The 44-sample stream recorded minimum available memory `6371221504`, minimum
disk free `3850231808`, maximum aggregate RSS `3521368064`, owned swap `0`,
and swap-I/O delta `117432320`; cleanup passed. There is no grant
verification, Selection, assembly, runner, execution, terminal, repeat
request, or oracle result. Raw output is `.codex-tmp/spec189-r132-launch.log`,
raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r132/`.
The candidate is unchanged; the next step is a space-preserving review of
duplicate immutable canonical artifacts followed by a fresh r133 run root.

The duplicate `canonical-repo-initializer.bin` and
`canonical-initializer.bin` were independently SHA-256 verified as
`413814d6166b5958623e45c085c502f56498e45769c93de45b8c16c2f61c62dd`, then
retained under both original paths with shared inode `4377094` and mode `600`.
No raw run, source, installed binary, or guard setting was removed; root free
space increased by `1503268864` bytes. This artifact operation does not alter
the protocol/resource verdict. The next fresh guarded candidate is
`two-provider-global-r133`.

The first r133 invocation stopped in preflight with
`MODEL_CANONICAL_INITIALIZER_SOURCE_NOT_IMMUTABLE`: the space-preserving
hard-link operation had applied mode `600` to the shared inode, while the
launcher requires the canonical initializer source to have no write bits. No
MiniNDN process or request chain started. Raw output is
`.codex-tmp/spec189-r133-launch.log`; the bounded repair restores read-only
mode on the shared immutable inode before retry.

r134 restored mode `444` and entered MiniNDN with a fresh run root. Both
Providers reached READY, but the unchanged host guard stopped at
`RESOURCE_BOUNDARY:diskFree` before requester ACK/Selection. The 39-sample
stream recorded minimum available memory `6469636096`, minimum disk free
`3847790592`, aggregate RSS peak `3527966720`, owned-swap peak `53248`, and
swap-I/O delta `54505472`; cleanup passed. There is no grant verification,
ACK/Selection, assembly, runner, execution, terminal, repeat request, or
oracle result. Raw output is `.codex-tmp/spec189-r134-launch.log`; raw run
root is `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r134/`.
This is a host artifact-placement boundary, not a protocol or qualification
result. Before r135, the preserved large staging artifacts must be checked
for exact digest identity and space-preserving deduplicated without deleting
the raw run evidence.

The r134 staging `.part` was independently verified as the canonical
initializer (`sha256:413814d6166b5958623e45c085c502f56498e45769c93de45b8c16c2f61c62dd`,
1,503,264,768 bytes) and atomically replaced by a hard link to inode `4377094`;
the original r134 path and bytes remain, now mode `444`. The same exact-match
operation was applied only to the verified r126/r127/r129 payload paths; the
r91 and r132 staging files had different digests and were preserved unchanged.
The canonical inode now has `84` links and host free space is
`9853657088` bytes. This is space-preserving evidence maintenance, not a
runtime or qualification result. The next step is a fresh r135 guarded run;
the new Repo staging allocation must still be observed as a resource gate.

r135 crossed the staging disk gate and ran the installed native candidate.
Both Providers reached READY, emitted signed `ACK_DECISION` offers, and
recorded `GRANT_VERIFICATION` at `BEFORE_ASSEMBLY`. The first native round
then failed at the requester stream boundary:
`NATIVE_STREAM_FAILED boundary=stream`, with Core message
`stream event gap exceeded retry budget`. The 348-sample resource record
ended in `drained`, with minimum available memory `2122010624`, minimum disk
free `4563804160`, RSS peak `8165933056`, owned-swap peak `117878784`, and
swap-I/O delta `117878784`. Provider-0 left an assembly cache cipher and ORT
profile, but no authenticated Selection, complete assembly/runner, execution,
terminal response, repeat request, or C++ oracle result is established.
Raw output is `.codex-tmp/spec189-r135-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r135/`.
This is a real production stream/assembly boundary, not a qualification
result. T005/T006/T007/T009 remain `PARTIAL`; next is a trace-enabled fresh
run to distinguish Selection-status/progress delivery from Provider assembly
stall before changing any timeout or guard limit.

Before the trace retry, r135's preserved Repo payload was independently
verified as the same canonical initializer digest and atomically hard-linked
to inode `4377094`; its original path and bytes remain, now mode `444`. The
shared inode has `86` links and host free space is `7570694144` bytes. This is
space-preserving evidence maintenance only; the r135 stream boundary and all
task statuses are unchanged.

r136 used the trace-enabled command and again started both Providers, but the
unchanged host guard stopped the fresh run at `RESOURCE_BOUNDARY:diskFree`.
The 283-sample record ended in `drained`: minimum available memory
`5972131840`, minimum disk free `3786682368`, RSS peak `4455481344`,
owned-swap peak `103866368`, and swap-I/O delta `103866368`; cleanup passed.
The requester trace recorded `43` stream-retry expressions and `42` timeout
callbacks for the selected Provider-1 event prefix, while Provider logs had no
Selection-status/progress trace and only the pre-assembly grant boundary.
Provider-0 retained a `752097499`-byte staging `canonical.onnx` plus root
metadata. No Selection/assembly completion, terminal response, repeat request,
or oracle result is established. Raw output is `.codex-tmp/spec189-r136-launch.log`;
raw run root is `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r136/`.
This is a host disk boundary that interrupted the trace experiment, not a
protocol result. Before r137, verify/deduplicate only exact immutable payload
matches, then use a fresh trace run with lifecycle tracing; do not alter guard
or stream limits.

The preserved r136 Repo payload was independently SHA-256 verified as the
canonical initializer and atomically hard-linked to inode `4377094`; its
original path and bytes remain, now mode `444`. The shared inode has `88` links
and host free space is `5289746432` bytes. The non-matching-sized r136
`canonical.onnx` staging artifact remains untouched. This is
space-preserving evidence maintenance only; r137 still requires fresh-run
trace and resource evidence.

Before r137, the nine preserved `752097499`-byte `canonical.onnx` staging
paths were independently verified against digest
`sha256:258f367628b69a66eccf0973386b2eb80e2fce74cd64c2172d4f00a6e06abb94`.
The r112 path was retained as inode `4853426`; the other eight historical
paths, including r136, were atomically hard-linked to that inode. All paths
and bytes remain available, the anchor now has `9` links, and root ext4 free
space is `11306508288` bytes. No open deleted file was found. This is
space-preserving evidence maintenance only; r137 still requires fresh-run
trace and resource evidence and no task status changes.

**B189-3 r137 native stream-gap boundary**: 2026-09-20 — r137 used a fresh
run root and the unchanged installed candidate and limits. The parent command
set `NDNSF_TIMELINE_TRACE=1`, but `env_for()` did not forward timeline-trace
variables to child processes; the launcher was repaired to forward timeline,
sample-rate, and stream-packet timeline controls, and the Python syntax check
passed. The run passed MiniNDN startup, both Provider READY/ACK decisions, and
both `GRANT_VERIFICATION boundary=BEFORE_ASSEMBLY` checks. Provider-0 reached
native assembly and retained a protected `752308868`-byte model cipher and a
`961277`-byte ORT profile; Provider-1 produced no Selection/assembly artifact.
The requester recorded `89` stream-retry lines and ended with
`NATIVE_STREAM_FAILED: stream event gap exceeded retry budget`. The 347-sample
stream ended `drained`: minimum available memory `2480660480`, minimum disk
free `6009753600`, RSS peak `8092606464`, owned-swap peak `197283840`, and
swap-I/O delta `590598144`; cleanup passed and the host guard did not stop the
run. No Selection, complete runner, execution, terminal, repeat request, or
C++ oracle result is established. Raw output is
`.codex-tmp/spec189-r137-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r137/`.
T005/T006/T007/T009 remain `PARTIAL`; the next changed gate is a fresh run
with the repaired child-process trace propagation, without changing limits.

**B189-3 r138 trace-propagation disk boundary**: 2026-09-20 — r138 was a
fresh run after the launcher repair and verified that timeline, sample-rate,
and stream-packet trace variables reached both Provider environments. It
passed MiniNDN startup, both Provider READY/ACK decisions, and both
`GRANT_VERIFICATION boundary=BEFORE_ASSEMBLY` checks. Provider-0 fetched a
`752097499`-byte staging `canonical.onnx`; no Selection, assembly completion,
runner, execution, terminal, repeat request, or C++ oracle result was
observed. The unchanged host guard stopped at `RESOURCE_BOUNDARY:diskFree`;
287 samples recorded minimum available memory `5828329472`, minimum disk free
`3729842176`, RSS peak `4453687296`, owned-swap peak `103759872`, and swap-I/O
delta `296140800`. Cleanup passed and the requester recorded cancellation at
the request boundary. Raw output is `.codex-tmp/spec189-r138-launch.log`; raw
run root is `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r138/`.
T005/T006/T007/T009 remain `PARTIAL`; next is exact space-preserving
deduplication of verified initializer/staging payloads, then a fresh run with
the same limits.

After r138, both r137/r138 canonical Repo initializer payload paths were
independently verified as digest
`sha256:413814d6166b5958623e45c085c502f56498e45769c93de45b8c16c2f61c62dd`
and retained on initializer inode `4377094`, now with `93` links. The r138
`canonical.onnx` staging path was independently verified as digest
`sha256:258f367628b69a66eccf0973386b2eb80e2fce74cd64c2172d4f00a6e06abb94`
and retained on inode `4853426`, now with `10` links. All paths and bytes
remain available; root ext4 free space is `7488299008` bytes. This is
space-preserving evidence maintenance only; no task status changes.

A further exact SHA-256 check found the r131 canonical Repo initializer
payload at the same digest; it was atomically hard-linked to initializer inode
`4377094`. The original r131 path and bytes remain, the inode now has `94`
links, and root ext4 free space is `8991244288` bytes. The r91/r122/r132
`.part` files had different digests and were deliberately left untouched.
This is space-preserving evidence maintenance only; no task status changes.

**B189-3 r118 resource boundary**: 2026-09-20 — after all system binaries
were updated to the verified build, the fresh r118 run passed candidate
identity, MiniNDN startup, both Provider readiness/offer decisions, and both
`NDNSF_DI_GRANT_VERIFICATION` checks at `BEFORE_ASSEMBLY`. The unchanged host
guard then stopped at `RESOURCE_BOUNDARY:MemAvailable`: minimum available
memory was `1080655872` bytes against `1610612736`, owned swap reached
`329965568` against `268435456`, swap-I/O delta reached `1314488320` against
`268435456`, and aggregate sampled RSS reached `9308581888` bytes. Cleanup
passed and no process remained. The requester only recorded cancellation;
there is no ACK_CLOSED/Selection, ASSEMBLY_STARTED, runner, execution,
terminal, repeat round, or oracle result. Raw output is
`.codex-tmp/spec189-r118-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r118/`.
Durable detail is in [r118 resource evidence](evidence/b189-r118-resource-boundary-20260920.md).
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Do not raise
the guard limits or count this as protocol PASS; next step is read-only native
preparation/assembly memory attribution and a bounded C++ ownership repair.

**B189-3 r117 digest invocation boundary**: 2026-09-20 — after installing
the current system binaries, the r117 command did not reach MiniNDN because
the manifest digest argument contained a typo (`...1b974...`) instead of the
verified `stage-manifest-qwen-v2.json` digest. Preflight returned
`MODEL_STAGE_MANIFEST_DIGEST_MISMATCH`; the preserved supervisor reports
`boundary: null`, `cleanup: PASS`, `returncode: 1`, and no remaining
processes. Raw output is `.codex-tmp/spec189-r117-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r117/`.
This is an invocation boundary, not a source, protocol, resource, or model
result. Durable detail is in [r117 digest evidence](evidence/b189-r117-digest-invocation-boundary-20260920.md).
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
a fresh r118 run with the exact verified manifest digest.

**B189-3 r116 preparation-source boundary**: 2026-09-20 — the fresh r116
candidate passed identity checks, MiniNDN Controller/Authority startup, and
reached the requester, but stopped before ACK/Selection because native
preparation reported `PREPARATION_SOURCE_UNAVAILABLE` with
`fallback initializer does not match pinned digest or size`. Both Provider
logs are empty; no request-chain protocol stage, resource qualification,
runner, execution, terminal, repeat round, or oracle result exists. The
preserved supervisor reports `boundary: null`, `cleanup: PASS`,
`returncode: 1`, and no remaining processes. Raw output is
`.codex-tmp/spec189-r116-launch.log`; raw run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r116/`.
The r116 requester config and published source manifest pin initializer
digest `sha256:413814d6166b5958623e45c085c502f56498e45769c93de45b8c16c2f61c62dd`,
and the preserved fallback file has the same digest; the changed gate is
installed requester/library provenance or the preparation fallback contract,
not a resource limit. Durable detail is in [r116 preparation evidence](evidence/b189-r116-preparation-source-boundary-20260920.md).
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
to install the current requester/authority/oracle targets from the verified
build tree, reverify their identities, then retry with a fresh run root.

**B189-3 r115 manifest preflight boundary**: 2026-09-20 — the first r115
launch used the older `stage-manifest-qwen-r99.json`, which has no required
`eosTokenIds`; candidate preflight stopped with
`MODEL_EOS_TOKEN_IDS_REQUIRED` before MiniNDN startup. The preserved r115
supervisor reports `boundary: null`, `cleanup: PASS`, `returncode: 1`, and no
remaining processes. Raw output is `.codex-tmp/spec189-r115-launch.log`, and
the preserved run root is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r115/`.
This is a launch-argument boundary, not a protocol or model result. Durable
detail is in [r115 manifest evidence](evidence/b189-r115-manifest-preflight-boundary-20260920.md).
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
a fresh run root with the prepared `stage-manifest-qwen-v2.json` and its
candidate digest.

**B189-3 r115 materialized-role graph repair checkpoint**: 2026-09-20 — the
post-selection compact role now carries an authenticated `materializedRole`
recipe flag. The complete canonical-source checker is skipped only for this
internal materialized-role path; shape inference, boundary extraction,
assembled-model checking, and ORT session creation remain required. The
affected build passed `434/434`; the material consumer selectors passed `2/2`;
the ONNX extraction/assembly/activation selectors passed `25/25`; all three
affected global targets were installed; and the refreshed native receipt
verified with SHA-256
`71cefd68e6bdf9ccdb58bb389e00f188b604a16b732e127239884a985052886b`.
Installed hashes are DI library
`c853a48832041fa7cdedd3c6f9033a97191b9861089ac42394da5a37cfd24461`, Provider
`fcc9d208b791d98c580dca03d5896395209f712d15893c1765615d4b4a3f9427`, and
Worker `548276efaa05acea21fe8ef8a5e0e056fafdf97e07958bc36e2049cffa8052fc`.
`ldd` resolves the installed Provider/Worker closure through `/usr/local` and
`/opt/onnxruntime` with no `not found`. Durable detail is in [r115 repair
evidence](evidence/b189-r115-materialized-role-graph-repair-20260920.md).
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
the fresh installed-runtime r115 MiniNDN two-Provider run.

**B189-3 r114 native graph boundary**: 2026-09-20 — the fresh installed-runtime
run passed candidate identity, MiniNDN startup, ACK closure, Selection, both
Provider grant checks, and Provider-0 `ASSEMBLY_STARTED`. It crossed the r112
resource boundary: the unchanged guard did not stop it, maximum owned swap was
`161386496` bytes against the `268435456`-byte limit, maximum RSS was
`6426677248` bytes, cleanup passed, and no process remained. Provider-0 then
failed at `DI_NATIVE_ONNX_GRAPH` after the material-fetch phase; Provider-1's
exact-Data terminal and the requester's stream failure are downstream. No
runner, successful ONNX execution, terminal success, repeat round, or oracle
result exists. Durable detail is in [r114 evidence](evidence/b189-r114-native-graph-boundary-20260920.md),
with raw run root under
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r114/`.
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
a read-only review of the exact native graph substage before any retry.

**B189-3 r113 material-backing release repair checkpoint**: 2026-09-20 —
after r112 exposed a host resource boundary, the native materializer now
scrubs/releases selected payload backing after every selected node and
initializer has been authenticated and copied into the protobuf model, before
final serialization. The conservative assembly budget is unchanged. The
affected build passed `434/434`; the material consumer selectors passed `2/2`;
and `Spec182OnnxActivation` passed `9/9` with the explicit worker fixture
directory. The affected global targets were installed and the refreshed native
receipt verified with SHA-256
`d548b4cb6356d6210ff79591b7f4f45a7d76f98231ac281c1f40550a549cdd4f`.
MiniNDN revalidation remains pending. Durable detail is in [r113 repair
evidence](evidence/b189-r113-material-backing-release-20260920.md). No task
checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is a fresh
r114 installed-runtime MiniNDN run.

**B189-3 r113 focused-selector invocation boundary**: 2026-09-20 — the
post-memory-repair native build passed `434/434`, but the first focused
selector invocation omitted `NDNSF_SPEC182_BIN_DIR=build-spec189-oracle`.
The selectors therefore stopped before their assertion bodies because the
worker binaries were not discoverable. This is an incomplete validation
command, not a source or MiniNDN result. Raw logs are
`.codex-tmp/spec189-r113-material-selector.log` and
`.codex-tmp/spec189-r113-onnx-activation.log`; durable detail is in
[r113 invocation evidence](evidence/b189-r113-selector-invocation-boundary-20260920.md).
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
to build the named worker fixtures if required, set the fixture path, rerun
the selectors, then install the changed candidate.

**B189-3 r112 owned-swap resource boundary**: 2026-09-20 — the fresh
installed-runtime run passed candidate identity, MiniNDN startup, ACK,
Selection, and both Provider grant checks. Provider-0 reached
`ASSEMBLY_STARTED`; Provider-1 reached `DEPENDENCY_FETCH` and later emitted a
downstream failed `TERMINAL` while Provider-0 was still assembling. The
unchanged host guard then stopped the supervised request at
`RESOURCE_BOUNDARY:ownedSwap`: first over-limit sample
`ownedSwapBytes=271503360` against `268435456`, with RSS
`5762756608` bytes and `swapIoDeltaBytes=634826752`. Cleanup passed and the
final sample drained to zero owned swap. No runner, successful ONNX execution,
terminal response, second round, or oracle result exists. Durable detail is in
[r112 evidence](evidence/b189-r112-owned-swap-boundary-20260920.md), with raw
run root under
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r112/`.
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
a read-only native assembly memory review before choosing a bounded fix or
candidate resource decision.

**B189-3 r111 digest-format preflight boundary**: 2026-09-20 — the fresh
candidate dispatch stopped before MiniNDN because the launcher requires
`sha256:<hex>` identities while the command supplied the stage-manifest
identity as bare hex, producing `MODEL_STAGE_MANIFEST_DIGEST_MISMATCH`. No
process, protocol, resource, model, or oracle result exists; the supervisor
cleaned up successfully and the final resource sample had zero owned swap.
Durable detail is in [r111 evidence](evidence/b189-r111-digest-format-boundary-20260920.md),
with raw run root under
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r111/`.
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next step is
a fresh r112 run root with the required `sha256:` prefixes on every digest.

**B189-3 r110 material-template repair checkpoint**: 2026-09-20 — the r109
shared-range decode defect was repaired: materialized template and node graph
parsing now uses `MaterialPayload::data()`/`byteSize()` rather than the legacy
`.bytes` member. The affected native build passed `434/434`; focused material
consumer selectors passed `2/2`; `Spec182OnnxActivation` passed `9/9`; all
affected global targets were installed; and the refreshed native receipt
verified with SHA-256
`a1128c3fe7001c3b996bfefa5246c9ae518866edf4385478865f0ab6ddc20ef0`.
No MiniNDN result or task checkbox changes; T005/T006/T007/T009 remain
`PARTIAL`. Durable detail is in [r110 evidence](evidence/b189-r110-material-template-repair-20260920.md).
Next step is a fresh installed-runtime MiniNDN run with a new run root.

**B189-3 r109 material-template boundary**: 2026-09-20 — the repaired
installed runtime passed candidate preflight, MiniNDN startup, ACK, Selection,
Provider grant verification, and `ASSEMBLY_STARTED`. Provider-0 verified 4,633
material payloads and 817 bundles, then failed at
`DI_NATIVE_ONNX_MATERIAL_TEMPLATE` because the new shared-range payload kept
its bytes in `MaterialPayload::backing` while the template decode still read
the legacy `.bytes` member. Provider-1's exact-Data dependency failure was
downstream. The resource guard did not stop the run; cleanup passed and the
final sample was drained with zero owned swap. Durable detail is in [r109
evidence](evidence/b189-r109-material-template-boundary-20260920.md), with
raw run root under `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r109/`.
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next changed
gate: use `data()`/`byteSize()` for every materialized template/node decode,
then rerun focused C++/installed checks and a fresh MiniNDN run.

**B189-3 r108 canonical-source preflight boundary**: 2026-09-20 — the new
changed-candidate launch stopped before MiniNDN because the command supplied
`candidate/canonical/canonical-qwen-external.onnx`, while the verified source
is in the campaign-level `canonical/` directory. The launcher reported
`MODEL_CANONICAL_SOURCE_MISSING`; no process, protocol, resource, model, or
oracle result exists. Raw output is `.codex-tmp/spec189-r108-launch.log`, with
durable detail in [r108 evidence](evidence/b189-r108-canonical-source-preflight-20260920.md).
This is a launch-argument boundary, not a product result. No task checkbox
changes; T005/T006/T007/T009 remain `PARTIAL`. Retry with a fresh run root and
the verified canonical source path.

**B189-3 r106 native material-budget boundary**: 2026-09-20 — the fresh
installed-runtime MiniNDN run passed candidate preflight, startup, ACK,
Selection, Provider grant verification, and native assembly admission on both
Providers. Provider-0 verified 5,453 material payloads totalling
`1511365303` bytes, then failed at the first native assembly boundary with
`DI_NATIVE_ONNX_MATERIAL_INITIALIZER`; the candidate's authenticated
`max_assembled_bytes=1571325451` is below the assembler's retained-material,
raw-initializer, and final-model working-set accounting. Provider-1's signed
dependency fetch failure followed Provider-0 termination. The resource guard
did not stop the run and cleanup passed (`returncode=1`, `boundary=null`, no
remaining processes). No runner, ONNX execution, terminal response,
second-round result, or oracle result exists. Durable detail is in [r106
evidence](evidence/b189-r106-native-material-budget-20260920.md), with raw
run root under `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r106/`.
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`. Next changed
gate: reconcile the planner's per-role assembly budget with the native
assembler's actual bounded working-set contract, then rerun static review,
focused native checks, installed identity checks, and a fresh MiniNDN run.

**B189-3 r107 native material-budget repair checkpoint**: 2026-09-20 — the
r106 gate was repaired in the C++ consumer and Qwen profile: selected external
initializer chunks now retain authenticated shared bundle ranges instead of
duplicating every bundle, and the scrubber clears shared backing allocations;
the profile now derives `max_assembled_bytes` from the assembler's conservative
working-set upper bound. The affected DI/integration and unit targets built,
the material consumer selectors passed 2/2, `Spec182OnnxActivation` passed
9/9, and affected global targets were installed with the canonical dependency
closure. The combined publisher selector still reaches the known staging-file
`Permission denied` environment boundary; it is not counted as a product
failure or PASS. Receipt verification passed with SHA-256
`6a3c8f5fa690f71161d4db389d0d23b2b54b3b769890a936b6a226b49baaa7f1`.
Durable detail is in [r107 evidence](evidence/b189-r107-native-material-budget-repair-20260920.md).
No MiniNDN result or task checkbox changes; T005/T006/T007/T009 remain
`PARTIAL`. Next step is a fresh changed-candidate installed-runtime run.

**B189-3 r105 installed-runtime preflight boundary**: 2026-09-20 — the
two-node stage count was corrected, but the fresh dispatch stopped before
MiniNDN at `MODEL_NODE_MAPPING_MISSING` because the command referenced
`candidate/qwen-node-mapping.json` instead of the existing canonical
`candidate/node-mapping.json`. No process, protocol, resource, or model result
exists. Raw output is `.codex-tmp/spec189-r105-launch.log`; use the existing
mapping only after verifying its digest and keep a new run root. No task
checkbox changes; T005/T006/T007/T009 remain `PARTIAL`.

**B189-3 r104 installed-runtime preflight boundary**: 2026-09-20 — the
first fresh installed-runtime dispatch stopped before MiniNDN because the
launcher defaulted to three `--stage-nodes` while the authenticated Qwen v2
manifest contains two stages: `--stage-nodes count must match stage manifest
and contain no duplicates`. No process, protocol, resource, or model result
exists. Raw output is `.codex-tmp/spec189-r104-launch.log`; retry with the
explicit two-node topology input and the same candidate/binary/resource
identities. No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`.

**B189-3 r103 assembly-timeout wiring checkpoint**: 2026-09-20 — source
review identified and repaired the standalone native Provider's missing
`assemblyTimeoutMs` assignment. The executable now exposes and logs a finite
independent assembly budget; the Qwen profile passes 900,000 ms while keeping
dependency-fetch/readiness/control budgets separate. The launcher now accepts
explicit installed binary paths, and `di-native-provider` has a canonical
BINDIR install path. Affected C++ build passed (`302/302`), the complete
`Spec182OnnxActivation` selector passed 9/9 after building its five worker
fixtures, and the affected Provider/worker/Authority/Requester/oracle targets
were installed with `/usr/local`/`/opt/onnxruntime` dependency closure checks.
See [r103 evidence](evidence/b189-r103-assembly-timeout-wiring-20260920.md).
The maintained build receipt was refreshed and independently verified
(`55a44f07b4760b6f607143e7797cff24effc202359f85d004031aec8404bf26c`). The
Controller was also installed so every MiniNDN executable can be supplied from
the canonical global runtime. A new
installed-runtime MiniNDN run with fresh run identity and unchanged
model/resource inputs is now the next gate. No task checkbox changes;
T005/T006/T007/T009 remain `PARTIAL`.

**B189-3 r103 worker-fixture build boundary**: 2026-09-20 — the retry build
for the missing `spec182-worker-tool-*` fixtures stopped before compilation
because the existing build-tree subdirectory
`build-spec189-oracle/tests/standalone/spec182-worker-tools` is owned by
`root:root` from an earlier root experiment. This is a build-environment
permission boundary, not a source or product result. Raw output is
`.codex-tmp/spec189-r103-worker-fixtures-build.log`; repair only that explicit
build subdirectory ownership, then rerun the same named targets. No task
checkbox changes; T005/T006/T007/T009 remain `PARTIAL`.

**B189-3 r103 focused-selector invocation boundary**: 2026-09-20 — the
affected `di-native-provider` and `unit-tests` targets compiled and linked
successfully after wiring the explicit assembly budget. The first
`Spec182OnnxActivation` selector invocation passed 7/9 cases but stopped in
the two cases that require the separately registered
`spec182-worker-tool-block` and `spec182-worker-tool-sigkill` fixtures; the
invocation had not built those targets. This is an incomplete validation
command, not a product result. The raw output is
`.codex-tmp/spec189-r103-onnx-activation.log`; build the named worker targets
and rerun the selector before installing or launching a new MiniNDN candidate.
No task checkbox changes; T005/T006/T007/T009 remain `PARTIAL`.

**B189-3 r102 native assembly-timeout checkpoint**: 2026-09-20 — the fresh
diagnostic run passed candidate/installed-runtime preflight and crossed the
r101 stream-only boundary: requester Selection commit and both Provider
Selection acceptance, `GRANT_VERIFIED`, and `ASSEMBLY_ADMISSION_REPORTED` were
observed. Provider-0 entered `ASSEMBLY_STARTED` and verified material fetches,
then failed with `DI_NATIVE_ONNX_ASSEMBLY_TIMEOUT`; Provider-1 failed its
dependent signed exact-Data fetch. The requester stream-gap error is recorded
as a downstream symptom. The unchanged resource guard did not stop the run
(`maxOwnedSwapBytes=116293632`, `minAvailableBytes=5158629376`,
`minDiskFreeBytes=26541301760`); cleanup passed, but there was no successful
runner, execution, terminal response, two-round output, or oracle result.
T005/T006/T007/T009 remain `PARTIAL`. See [r102 evidence](evidence/b189-r102-native-assembly-timeout-20260920.md).
Source/timing diagnosis now identifies the first boundary: Provider-0 entered
assembly at `1789912424.051127`, verified the last material payload at
`1789912603.290546`, and failed at `1789912603.579023`. The native Provider
executable did not wire `NativeCanonicalOnnxAssemblerOptions.assemblyTimeoutMs`,
leaving its 30,000 ms default active while the assembler started that deadline
before synchronous material fetches. The worker therefore observed an expired
deadline after the fetch and reported `DI_NATIVE_ONNX_ASSEMBLY_TIMEOUT`.
Next changed gate: add an explicit finite assembly budget independent from
dependency-fetch/readiness/control budgets, log its effective value, then run
read-only static review, affected build/install/identity checks, and a new
run-scoped MiniNDN candidate. This does not close any task or qualify the
protocol; T005/T006/T007/T009 remain `PARTIAL`.

**B189-3 r101 native stream-gap checkpoint**: 2026-09-20 — the candidate
preflight and real MiniNDN startup passed; both Providers emitted
`ACK_DECISION` and `GRANT_VERIFIED` before assembly, and Repo publication
created the run-scoped encrypted manifests. The first C++ requester request
failed with `NATIVE_STREAM_FAILED` / `stream event gap exceeded retry budget`
at the `stream` boundary; supervisor cleanup passed, and the guard did not
stop for owned swap. No Selection, placement-bound fetch, assembly, runner,
terminal response, or oracle result was recorded. Diagnose the exact stream
event publication/retention/retry boundary before r102. See [r101 evidence](evidence/b189-r101-native-stream-gap-20260920.md).
T005/T006/T007/T009 remain `PARTIAL`.

The diagnostic-only launcher adjustment passed Python syntax and diff checks:
`env_for` now forwards the existing `SPEC175_TRACE` control to MiniNDN child
processes. It does not change retry budgets or acceptance semantics; r102 is
the first runtime using this observation control.

The requested read-only DeepSeek review fallback was attempted but the
maintained `tools/ai/deepseek_delegate.py` entrypoint is absent in this
checkout. Primary-agent CodeGraph/source review is the fallback; no delegate
opinion is treated as evidence and no source change is justified by that
failed tool invocation.

**B189-3 r101 install invocation boundary**: 2026-09-20 — the first
candidate-install invocation passed the shell script to Python and stopped with
`SyntaxError` before build/install or MiniNDN. The raw output is
`.codex-tmp/spec189-r101-install.log`; retry with the script interpreter. No
task is complete and T005/T006/T007/T009 remain `PARTIAL`.

**B189-3 r100 EOS-token preflight checkpoint**: 2026-09-20 — the repaired
Qwen stage-manifest copy passed family/schema/stage validation but the launcher
stopped before MiniNDN at `MODEL_EOS_TOKEN_IDS_REQUIRED`; the older manifest
does not carry the prepared stop-token contract. No task checkbox changes and
no protocol/resource result. Recover EOS IDs from the immutable tokenizer/model
metadata, validate a new manifest copy, and retry under a new run ID. T005/
T006/T007/T009 remain `PARTIAL`; see [r100 EOS evidence](evidence/b189-r100-eos-token-preflight-boundary-20260920.md).

**B189-3 r99 stage-manifest preflight checkpoint**: 2026-09-20 — the new
launcher invocation passed the refreshed candidate identity arguments but
stopped before MiniNDN at `ValueError: stage manifest requires an explicit
modelFamily`; the existing candidate stage manifest is from an older contract.
No task checkbox changes and no protocol/resource result. Recover and verify
the immutable Qwen stage manifest, then retry under a new run ID. T005/T006/
T007/T009 remain `PARTIAL`; see [r99 stage-manifest evidence](evidence/b189-r99-stage-manifest-family-boundary-20260920.md).

**B189-3 r99 native binding compile checkpoint**: 2026-09-20 — the maintained
native-build helper rebuilt the Waf/examples closure, then its forced Python
binding compile stopped at `di_bindings.cpp:688` because the new shared byte
buffer wrapper did not accept the old iterator-pair `optional.emplace` call.
No task checkbox changes and no MiniNDN process started. Replace that call
with explicit vector construction, refresh the helper receipt, and rerun
independent verify. See [r99 binding boundary evidence](evidence/b189-r99-native-build-binding-boundary-20260920.md).

**B189-3 r99 native identity stale-source checkpoint**: 2026-09-20 — the
zero-copy producer material-range change passed focused C++ selectors and the
target-scoped DI install completed, but the immediate maintained native
identity verification stopped before MiniNDN at
`SPEC180_NATIVE_IDENTITY_REJECTED: STALE_SOURCES` because the old build receipt
predated the source change. No task checkbox changes. Refresh the receipt with
the maintained native-build helper, rerun independent verify, and recompute
the full candidate digest map before a new MiniNDN run. T005/T006/T007/T009
remain `PARTIAL`; see [r99 stale-source evidence](evidence/b189-r99-native-verify-stale-sources-20260920.md).

**B189-3 r98 owned-swap resource checkpoint**: 2026-09-20 — the fresh root
r98 invocation satisfied the installed-candidate, full `sha256:` digest, PATH,
and resource preflight gates and started MiniNDN. Both Providers reached
`READY`, `ACK_DECISION`, and `GRANT_VERIFIED(BEFORE_ASSEMBLY)`, then the
maintained host guard stopped the request at
`RESOURCE_BOUNDARY:ownedSwap` because the run exceeded the unchanged 256 MiB
owned-swap limit. Cleanup passed; no Selection, selected-material fetch,
assembly, runner, handoff, terminal, token, or repeat result was observed.
T005/T006/T007/T009 remain `PARTIAL`; no checkbox changes. See [r98 evidence](evidence/b189-native-r98-owned-swap-boundary-20260920.md).
The next gate is a new resource-controlled run after inspecting/reducing the
working set within the production path, while retaining the complete identity
contract and maintained resource limits.

**B189-3 r97 candidate digest-format checkpoint**: 2026-09-20 — root r97
passed the host resource guard but stopped before MiniNDN because the command
passed bare hexadecimal digests while `require_file_digest()` requires the
`sha256:<hex>` form. No protocol stage started. The changed gate for r98 is a
complete prefixed digest map verified against the launcher helper; T006/T007/
T009 remain `PARTIAL`. See [r97 evidence](evidence/b189-native-r97-digest-format-boundary-20260920.md).

**B189-3 r96 launcher privilege checkpoint**: 2026-09-20 — after restoring
the declared swap contract, r96 passed the host resource guard but the
ordinary-user invocation stopped before MiniNDN at
`MININDN_REQUIRES_ROOT: run this script with sudo -E`. No protocol stage
started. The maintained root boundary is now the changed gate for r97; keep
all candidate digests and resource thresholds unchanged. T006/T007/T009 remain
`PARTIAL`; see [r96 evidence](evidence/b189-native-r96-launcher-root-boundary-20260920.md).

**B189-3 r95 resource-contract checkpoint**: 2026-09-20 — after r94's
`swapIo` boundary, the host swapfile was temporarily disabled to eliminate
swap I/O, but the maintained guard correctly rejected r95 at
`RESOURCE_BOUNDARY:SwapFree` because its unchanged contract requires 512 MiB
free swap. No MiniNDN process or protocol stage started. Restore the declared
swap capacity, verify free swap/available memory/disk and a stable `vmstat`
window, then retry as a new r96 run. T006/T007/T009 remain `PARTIAL`; see
[r95 evidence](evidence/b189-native-r95-resource-boundary-20260920.md).

**B189-3 r92-r94 launcher/resource checkpoints**: 2026-09-20 — r92 stopped
before MiniNDN because the launcher invocation omitted required installed
binary/build-receipt digests; r93 passed those identity checks but lacked
`/usr/local/bin` and stopped in MiniNDN cleanup at `nfd-stop`; r94 crossed the
PATH repair and stopped before topology startup at
`RESOURCE_BOUNDARY:swapIo`. All three attempts are preserved as
`UNQUALIFIED` launcher/host boundaries and do not advance T006/T007/T009.
The next changed gate is a sustained host resource preflight with new run ID,
zero new swap-in/out during the guard window, and materially more than the
four-GiB disk floor. See [r92](evidence/b189-native-r92-preflight-boundary-20260920.md),
[r93](evidence/b189-native-r93-preflight-boundary-20260920.md), and
[r94](evidence/b189-native-r94-resource-boundary-20260920.md).

**B189 target-bound lifecycle/r91 checkpoint**: 2026-09-20 — 官方只读
`review-agent` 对 Provider cache/runner lifetime、canonical artifact path 和
assembler final-directory cleanup 的冻结 r8 快照返回 `STATIC_PASS`。DI 目标闭包
以系统依赖和 Waf `-j4` 编译通过（556/556），Provider lifecycle、stream、prepare、
material consumer 和 production integration 的 C++ 定向 selector 均通过；维护的
target-scoped install helper 也完成了 `ndnsf-distributed-inference` 安装。一次无范围
的根 `waf install` 调度 2284 tasks 后中止，已登记为安装边界，不能当产品失败。
使用全新安装候选的真实 Qwen r91 已启动 MiniNDN 和两个 native Provider，并都达到
`NDNSF_DI_NATIVE_PROVIDER_READY`；随后宿主 `diskFree` 低于 4 GiB 守护阈值而停止，
cleanup PASS，但没有 ACK、Selection、assembly、runner、terminal 或 token。T006/T007/
T009 继续 `PARTIAL`，下一次必须在更大磁盘余量和新 run ID 下重试。完整静态、构建、
资源边界和五 lane 记录见 [r8/r91 evidence](evidence/b189-memory-lifecycle-r8-and-r91-20260920.md)。
API reference 已重新生成当前声明；`Design/test_design_state.py` 通过，但完整
`Design/verify-api-reference.py` 仍因混合工作树的源码快照漂移和旧 target reference
失败，故文档交付门仍为 `PARTIAL`。
盘点确认 r91 的首要宿主原因是重复 Qwen run staging、Git 临时 pack、CodeGraph
索引和多份旧安装候选；单个 base SIF 约 4.09 GB，不是主要增长来源。当前磁盘审计
及 deleted-but-open Codex 文件句柄记录在同一份 [r8/r91 evidence](evidence/b189-memory-lifecycle-r8-and-r91-20260920.md) 中。
随后按 allow-list 清理了未被证据引用的旧 Qwen runs、四个 superseded 安装候选和 Git
临时 pack；CodeGraph 已在忽略实验暂存后重建。清理明细见
[cleanup evidence](evidence/cleanup-20260920.json)，当前工作树仍保持 `IN_PROGRESS`，未
改变 Qwen 全链路验收状态。

**B189 shared-chain diagnosis checkpoint**: 2026-09-20 04:34 -0500 — 重新对照 YOLO 与 Qwen 真实运行边界：
两者共用 prepare/Repo/request/ACK/Selection/grant ingress，r47/r70 已证明 Qwen 能进入
ACK、Selection、授权验证和 selected-material fetch；因此当前阻断不是共同入口不兼容。
YOLO 的 `NATIVE_POSTPROCESS` 通过不覆盖 Qwen 的分阶段材料、hidden-state handoff、ORT
runner 和长时间 assembly。r30–r33 的 `RESOURCE_BOUNDARY:swapIo` 属于宿主资源门，另行
保留；r70 的首个生产缺陷是 terminal consumer 未接受 nonterminal Provider 的
authenticated progress，导致 assembly/dependency 等待期间 stream gap。当前 progress
binding 修复已有 C++ selector/静态证据，但尚未用新安装候选完成真实重跑；T006/T007/T009
保持 `PARTIAL`，不能用 YOLO、timeout 或静态 PASS 关闭。
本次 Spec/plan/tasks/batch/traceability 修订的五 lane 记录见
[boundary revision evidence](evidence/b189-spec-revision-20260920.md)。

**B189 memory/SmolLM2 checkpoint**: 2026-09-20 — C++
`ProviderArtifactCache` 生命周期修复已通过官方只读 `STATIC_PASS`，并以 `-j3`
完成受影响目标编译；stopped-active/replacement/cleanup 及 SmolLM2 catalog/planner
selector 均通过。修正后的 exporter 重新生成真实
`HuggingFaceTB/SmolLM2-135M` 两阶段 ONNX：326,168,079 与 326,171,617 bytes，
manifest 含 `modelFamily=llama`、EOS `[0]`、层范围和匹配 SHA-256，整模型/分阶段
top-token 均为 28。`plan_pipeline.py` 已按 family 选择默认模型并拒绝 Qwen/SmolLM2
错绑。stage 文件仅是 exporter-side 契约制品；生产 Provider 必须沿
`prepare → Repo → request(reference/input) → ACK → Selection → selected material
fetch/assembly/execute → terminal` 运行。当前冻结范围的官方复审为 `STATIC_PASS`；完整
两 Provider MiniNDN 尚未运行，任务保持 `PARTIAL`。见
[本批证据](evidence/b189-memory-smollm-batch-20260920.md)。

**B189 native memory-lifecycle audit**: 2026-09-20 — 静态检查发现 prepare publication
原来同时保留完整 canonical source/initializer 与 material manifest；post-Selection
assembly 在 role model 已物化后仍把 selected payloads/manifest 带入 worker。当前改动用
不可变 `shared_ptr` 原子替换释放 prepare 的 full source，并在
`materializeNativeCanonicalModel()` 后释放 Provider 侧 selected material。DI 与
`spec189-canonical-publisher` 编译成功，material-only publication、owning source
snapshot、queued cancellation 和 source-after-cancel rollback 定向 selector 均通过；
完整 `Spec182CanonicalPublisher` 在可写 run-scoped staging 下 14/14 通过，默认
root-owned `/tmp` 目录的权限失败保留为宿主配置边界。官方 `review-agent` 复审现已返回
`STATIC_PASS / TESTS_DEFERRED`；这只关闭本次内存
生命周期静态门，不关闭任何产品任务。
随后又收窄 `ModelPreparationCache` 的 owning source 作用域，确保 publication 前旧
full source 不被局部 `shared_ptr` 保持；增量重建 45.967s，两个定向 selector 再次通过。
完整覆盖、失败边界和下一步见
[memory lifecycle static audit](evidence/b189-memory-lifecycle-static-20260920.md)。

**B189 architecture-boundary correction**: 2026-09-20 — 复核确认 exporter 生成的
两阶段 ONNX 只用于输入/输出、EOS、层范围和 top-token 契约校验；Provider manifest
不再携带 exporter-local stage path，真实 C++ Provider 仍须在认证 Selection 后从
canonical Repo publication 获取被分配材料并组装 runner。与此同时，当前
`NativeQwenLayerSplit` 仍从 catalog 读取单个预声明 layer-range 候选，ACK 后决定
的是 Provider placement，而不是从多个切分候选中重新决定 range。文档已将此状态从
“ACK determines partition”修正为“ACK constrains placement; Selection authorizes
materialization”；真正的 ACK-driven partition 仍是未完成能力，不能由 stage 文件
或静态 placement 通过替代。产品和 MiniNDN 状态不变，仍为 `PARTIAL`。

**B189 exporter/reuse contract repair**: 2026-09-20 — 只读复审发现并修正维护 helper
的三个契约缺口：runtime manifest 的 `stages` 实际是整数而非 stage 列表；service/runtime
的 canonical revision 在 loader 解析 commit hash 后必须一致；EOS token 校验不能接受
Python `bool`。ONNX `SplitArtifact` 现在显式标记 `materialization=exporter-contract-only`，
提醒共享 Python policy 不能把本地 stage 文件当成 native Provider 输入。`py_compile` 与
限定 `git diff --check` 通过；随后按当前全局 C++/DI 安装重建 Python binding，
Spec175 ONNX boundary 与 Spec107 artifact reuse 定向测试共 `18 passed`。Python
provider 的 service-policy、eager preload、can-prepare 和 Selection preparation 路径均拒绝
`exporter-contract-only` stage；显式 local artifact 标为 `operator-local-unverified`，
没有 Repo registration 时拒绝。真实 MiniNDN 和产品状态不变。

**B189-3 host-native r70 progressed stream checkpoint**: 2026-09-19 — the
verified r65 candidate passed launch/resource gates, closed ACK and committed
Selection. Provider 1 reached `GRANT_VERIFIED`,
`ASSEMBLY_ADMISSION_REPORTED`, `EXECUTION_ENTERED` and `DEPENDENCY_FETCH`; Provider
0 emitted repeated authenticated selected-material `begin/returned/verified`
records. The requester still stopped at `NATIVE_STREAM_FAILED / stream event gap
exceeded retry budget` while assembly/dependency reads were active. Cleanup and
resource floors passed, but no runner, terminal response or token was observed.
The raw run and boundary diagnosis are preserved in [r70 evidence](evidence/b189-native-r70-progressed-stream-boundary-20260919.md).
This changes the next gate to a reviewed Core collaboration progress binding:
the terminal stream consumer must accept only exact `{provider,
role-specific assembly-progress operationId}` pairs from the committed
Selection, while preserving provider/member identity and monotonic freshness.
T003/T006/T007 remain `PARTIAL` and T009 remains blocked.

**B189-3 host-native r66 preflight checkpoint**: 2026-09-19 — the exact
candidate digests passed, but the root runner lacked the user-installed
`ndn.encoding` module while decoding Provider PIB keys and stopped with
`provider key prefix unavailable` before MiniNDN. The raw run is retained;
r67 supplies the explicit maintained MiniNDN Python path. T003/T006/T007
remain `PARTIAL` and T009 remains blocked. See [r66 preflight evidence](evidence/b189-native-r66-preflight-boundary-20260919.md).

**B189-3 host-native r65 preflight checkpoint**: 2026-09-19 — the reviewed
diagnostic candidate compiled and the 15-case C++ CollaborationStatus selector
passed, but the first r65 launcher command supplied a truncated node-mapping
digest and stopped at `MODEL_NODE_MAPPING_DIGEST_MISMATCH` before MiniNDN.
The raw supervisor record is retained; r66 must use the exact unchanged digest
and a new run directory. T003/T006/T007 remain `PARTIAL` and T009 remains
blocked. See [r65 preflight evidence](evidence/b189-native-r65-preflight-boundary-20260919.md).

**B189-3 host-native r64 checkpoint**: 2026-09-19 — the system-installed
candidate passed all preflight gates, MiniNDN startup, both Provider readiness,
ACK/Selection and grant verification. The requester then stopped at
`NATIVE_STREAM_FAILED / stream event gap exceeded retry budget`; no assembly,
runner, execution, terminal response or model token was observed. Resource and
cleanup gates passed. The changed gate is the post-grant admission/progress
handoff; T003/T006/T007 remain `PARTIAL` and T009 remains blocked. See [r64
stream-boundary evidence](evidence/b189-native-r64-stream-boundary-20260919.md).

**B189-3 host-native r63 preflight checkpoint**: 2026-09-19 — stage identity
and two-node topology checks passed, then a hand-typed controller digest missed
one character and stopped the launcher before MiniNDN. The installed binary and
manifest agree; no product status changed. See [r63 preflight evidence](evidence/b189-native-r63-preflight-boundary-20260919.md).

**B189-3 host-native r62 preflight checkpoint**: 2026-09-19 — the corrected
stage digest passed, then the launcher rejected the default three-node list
against the candidate's two stages. No process started and no product status
changed; r63 will pass exactly two stage nodes. See [r62 preflight evidence](evidence/b189-native-r62-preflight-boundary-20260919.md).

**B189-3 host-native r61 preflight checkpoint**: 2026-09-19 — the new run was
rejected before MiniNDN because the supplied stage-manifest digest was stale.
No process or protocol path started; the actual unchanged candidate digest was
recomputed and recorded. This launch-only failure does not change T003/T006/T007
or T009 status. See [r61 preflight evidence](evidence/b189-native-r61-preflight-boundary-20260919.md).

**B189-3 host-native r60 resource checkpoint**: 2026-09-19 — the system-installed
Spec189 runtime passed candidate/linker preflight, but the fresh
`two-provider-global-r60` attempt stopped at `RESOURCE_BOUNDARY:diskFree` before
ACK/Selection. Supervisor cleanup passed and the raw run is retained. The four
GiB disk guard is unchanged; the next retry must remove duplicate immutable
initializer storage or warm the Repo without deleting raw evidence. 43 canonical
initializer paths and 12 confirmed Repo payload paths were hard-linked; the
identity-mismatching r60 staging `.part` was retained. No provider assembly,
terminal result or qualification was observed. T003/T006/T007 remain `PARTIAL`;
T009 remains blocked. See [r60 disk-boundary evidence](evidence/b189-native-r60-disk-boundary-20260919.md).

**B189-3 host-native r59 checkpoint**: 2026-09-19 — removed 808 rebuildable
host build intermediates (4.3 GiB) while retaining raw r44–r59 evidence and
the six native binary hashes. The host-native retry passed the resource guard;
both Providers reached `READY`, ACK offers and `GRANT_VERIFICATION`, then the
requester stopped at `NATIVE_STREAM_FAILED / stream event gap exceeded retry
budget` before any assembly admission marker. Cleanup passed and no SIF,
Apptainer or Tiger runtime was involved. T006/T007 remain `PARTIAL`; T009
remains blocked. See [r59 evidence](evidence/b189-native-r59-20260919.md).

**B189-3 T006-R1 reporter-sequence checkpoint**: 2026-09-19 — the executable
runner factory now passes the Selection-owned assembly progress counter to its
post-admission reporter. The frozen repair passed read-only `STATIC_PASS`; the
first global-r3 compile exposed only missing `ndnsf::di::` test qualifiers,
which was retained in the raw build log and corrected under review. The rerun
compiled `unit-tests` and `di-native-provider` 302/302 with Waf `-j4`, and the
new reporter-contract selector plus all 15 `CollaborationStatus` cases passed.
This proves only the sequence contract; a fresh real MiniNDN run is still
required and T006/T007/T009 remain `PARTIAL`.

**B189-3 admission-sequence repair / r58 checkpoint**: 2026-09-19 — the
v4 frozen scope passed official read-only `STATIC_PASS`. A Selection-scoped
runtime sequence counter now survives Provider runner-factory copies and
rebuilds; local C++ lifecycle 1→2→3 and the complete assembly suite passed
9/9 after building the worker from the repository root. The fresh
`two-provider-global-r58` run stopped before ACK/Selection at
`RESOURCE_BOUNDARY:diskFree`; cleanup passed, memory stayed above 4.24 GiB,
and no protocol or model result was observed. T006/T007 remain `PARTIAL` and
T009 remains blocked by B189-3. See [admission sequence/r58 evidence](evidence/b189-admission-sequence-r58-20260919.md).

**B189-3 real Qwen r56 checkpoint**: 2026-09-19 — the fresh
`two-provider-global-r56` candidate reached both Provider `READY`, ACK and
`BEFORE_ASSEMBLY` grant verification, then the requester stopped with
`NATIVE_STREAM_FAILED` / `stream event gap exceeded retry budget`. Neither
Provider emitted `ASSEMBLY_STARTED`, `ROOT_VERIFIED`, `RUNNER_READY`, execution,
terminal output or a stage result. Supervisor cleanup passed; resource samples
showed zero swap-I/O delta and the run is not a resource boundary. This is the
first proven post-grant stream-liveness boundary, not a Repo, ORT or model
failure. The repaired candidate now reports authenticated `ASSEMBLY_STARTED`
before root fetch; its immutable scope passed read-only review, rebuilt with
Waf `-j3` in 35.411s, and the C++ lifecycle/assembly suites passed 15/15 and
9/9. A fresh real run with the rebuilt binaries is the next gate; no further
component expansion or timeout increase is planned before that run. See
[r56 evidence](evidence/b189-real-qwen-r56-20260919.md).

**B189-3 progress/heartbeat checkpoint**: 2026-09-19 — the frozen v6
`review-agent` snapshot passed `STATIC_PASS` for authenticated post-Selection
assembly progress. The affected DI/unit/integration/assembly-worker closure
rebuilt from global-r3 with Waf `-j3`; lifecycle (15/15), same-provider
multi-role streamed, D2b streamed, and worker-backed assembly selectors passed.
The first full `Spec175NativeAssembly` run exposed a fixture missing
`metadata.canonicalSourceDigest`/`canonicalSourceBytes`; after read-only
review, the test-only fixture correction rebuilt with `-j3` and the complete
suite passed 9/9. No real Qwen/MiniNDN retry, terminal output, resource drain or
qualification was run in this unit.
See [progress/heartbeat evidence](evidence/b189-r4-progress-heartbeat-20260919.md)
and [failure log](../../docs/failure-log.md).

**B189-3 real Qwen r53 diagnostic checkpoint**: 2026-09-19 — a fresh run with
runtime timing, assignment-fetch tracing, large-fetch timing and dependency
object tracing enabled carried the repaired `900000 ms` dependency budget into
both Providers. Both reached readiness, ACK decisions and `BEFORE_ASSEMBLY`
grant verification; Provider 0 created an assembly staging `root.json`. The
requester still failed with `NATIVE_STREAM_FAILED` / `stream event gap exceeded
retry budget`, while the supervisor recorded `cleanup=PASS` and no remaining
processes. No runner, output, terminal response or completed provider stage
marker was observed; the enabled diagnostics did not expose a completed stage
marker, so this is an observability/liveness boundary rather than proof of
Provider idleness or ORT failure. No qualification is claimed. The next unit
must be a reviewed authenticated progress/heartbeat or bounded admission-state
transition with a C++ counterexample before another real retry. T003/T005/
T006/T007/T009 remain `PARTIAL`. See [r53 diagnostic evidence](evidence/b189-real-qwen-r53-diagnostic-20260919.md).

**B189-3 real Qwen r49-r52 liveness checkpoint**: 2026-09-19 — r49 and r50
stopped at command preflight (provider digest, then tokenizer digest); r51
reached MiniNDN cleanup but used a PATH without `/usr/local/bin`. The corrected
r52 run passed preflight/startup, carried the repaired `900000 ms` dependency
fetch budget into both native Providers, and reached both ACK decisions,
Selection assignment publication, grant verification and post-Selection
assembly staging. The requester still failed before a provider terminal event
with `NATIVE_STREAM_FAILED` / `stream event gap exceeded retry budget`;
`cleanup=PASS`, but no runner, output, terminal response or qualification was
observed. This confirms the timeout wiring is present but leaves a real
stream-liveness versus silent assembly contract. No blind timeout increase is
approved; next work must be a reviewed progress/heartbeat or bounded admission
state transition with a C++ assertion and runtime-timing evidence. T003/T005/
T006/T007/T009 remain `PARTIAL`. See [r49-r52 evidence](evidence/b189-real-qwen-r49-r52-20260919.md).

**B189-3 provider dependency-timeout wiring checkpoint**: 2026-09-19 — the
provider CLI's `--repo-fetch-timeout-ms` is now wired to a dedicated
`dependencyFetchTimeoutMs`; readiness and conversation-control deadlines retain
their independent bounded `fetchTimeoutMs`. Official read-only review of the
final frozen snapshot passed `STATIC_PASS`. The existing global-r3 tree rebuilt
the affected DI library, provider executable and unit-tests with repository Waf
`-j4`; the timeout-budget C++ selectors passed, including an environment
override regression proving that only dependency fetch changes. This closes a
configuration-wiring defect exposed by r48; it does not establish a new Qwen
run, runner, output, terminal response or qualification. T003/T006/T007/T009
remain `PARTIAL`. See [provider timeout evidence](evidence/b189-provider-timeout-20260919.md).

**B189-1c external initializer range fast-path checkpoint**: 2026-09-19 — the
frozen ONNX assembler diff received official read-only `STATIC_PASS`. The shared
external range validator and direct numeric-range digest path preserve exact
identity semantics while avoiding a temporary large `TensorProto` and normalized
copy; INT4/UINT4 retain the prior fallback. The global-r3 unit target rebuilt
with `-j4` in 25.645s and `Spec182OnnxIdentity` passed 13/13. The affected DI,
requester, provider, worker and oracle targets rebuilt with `-j4` in 4m22.913s.
Real Qwen r44 reached both Provider ACK offers and `GRANT_VERIFICATION` with
zero swap-I/O delta, then stopped at `NATIVE_STREAM_FAILED` / provider stream
event-gap; cleanup passed and no terminal response or qualification was observed.
This removes the former r39 preparation-timeout boundary but does not close T003,
which remains `PARTIAL`. See [ONNX fast-path evidence](evidence/b189-onnx-fastpath-20260919.md).

**B189-3 real Qwen r47 diagnostic checkpoint**: 2026-09-19 — a fresh run with
provider assignment/fetch/runtime diagnostics reached the complete post-Selection
entry boundary in one request. Both Providers fetched their assignments and
reached `GRANT_VERIFIED` and `EXECUTION_ENTERED`; Stage/0 reached
`ASSEMBLY_STARTED`, fetched and verified the root and material manifests, and
began the authenticated material-receipt fetch. Stage/1 entered dependency fetch
and retried the absent Stage/0 `hidden_states` manifest. The requester then
expired its stream-event-gap retry budget before any receipt completion, runner
ready marker, stage output or terminal response. Cleanup passed. This narrows the
next boundary to the stream wait/heartbeat contract versus the provider receipt
fetch; it does not prove a receipt or ORT failure and does not close T003/T006/
T007/T009. See [r47 evidence](evidence/b189-real-qwen-r47-20260919.md).

**B189-3 real Qwen r48 liveness checkpoint**: 2026-09-19 — after wiring
`interestLifetimeMs=5000` and `maxEventRetries=8` through the native requester,
a fresh run reached both Providers' ACK/Selection/grant/execution boundary.
Stage/0 verified the root and material manifests and a 3,992,638-byte material
receipt, then continued selected-material assembly without `RUNNER_READY`;
Stage/1 retried the Stage/0 `hidden_states` dependency and failed its terminal
dependency fetch at about 30 seconds. The requester subsequently reported
`NATIVE_STREAM_FAILED`/stream-event-gap. Bundle, candidate, machine, model,
MiniNDN startup and cleanup passed, but workload failed and no output or
qualification was observed. This proves the wait options are carried into the
real request and moves the first observed boundary to long post-Selection
materialization versus the dependency/stream liveness windows; it does not
justify a blind timeout increase or prove receipt/ORT failure. T003/T006/T007/
T009 remain `PARTIAL`. See [r48 evidence](evidence/b189-real-qwen-r48-20260919.md).

**B189-1b r21/r22 chunked-material checkpoint**: 2026-09-19 — external
initializers are published as a bounded header plus ordered raw chunks and are
reassembled by the native post-Selection consumer. Official read-only review
passed for the production/schema, C++ round-trip oracle, and receipt/inline
boundary snapshots (r9/r11/r18). Root Waf `-j4` rebuilt the worker (26.348s),
unit target (31.556s), and affected integration target (24.202s). The C++
chunk round-trip selector and both receipt/selected-fetch selectors passed.
The final combined `unit-tests,integration-tests,di-native-assembly-worker`
Waf build after the last source change completed in 23.498s.
The negative cases include parse-reservation budget exhaustion, receipt
identity/duplicate-key rejection, and oversized inline root rejection before
encrypted fetch. This is a local material/consumer boundary only; T003 remains
`PARTIAL`, and real protected Qwen preparation, ACK/Selection, two-provider
execution, output, drain, MiniNDN and Tiger qualification remain open. See
[chunked material evidence](evidence/b189-material-publication-20260919.md).

**B189-1b r20 local verification checkpoint**: 2026-09-19 — the material
consumer fixture repair passed official read-only review r19
(`STATIC_PASS`, snapshot SHA-256
`92d475dfc3c1bf8a77150c52ac9696dccf4c7ea7d101635f996bceb28e416c6a`). The
affected `integration-tests` target rebuilt with root Waf `-j4` in 22.803s;
the real worker bundle selector and selected-payload budget selector passed.
The related GrantIssuer and protected publisher selectors passed. The Repo
publication selector first exposed a stale assertion that required fewer Repo
objects than payloads; after the r20 review (`STATIC_PASS`, snapshot SHA-256
`448a01c7e7848b46bd23caba73fcac3e7cd10a6ebd7f5466eaa1bf95cbb41aa1`),
`unit-tests` rebuilt with root Waf `-j4` in 26.617s and the Repo, GrantIssuer,
and publisher selectors passed. Repo keeps one independently addressable
range-store object per material payload; protected NDN publication owns bundle
coalescing. The root NDNSF Waf builds only NDNSF-owned targets and consumes
the installed NAC-ABE SDK; NAC-ABE remains owned by its own build system.
This closes only local material-publication/consumer evidence. T003, real
Qwen preparation, ACK/Selection, Provider execution, MiniNDN/Tiger and
qualification remain `PARTIAL`/open. See [material publication evidence](evidence/b189-material-publication-20260919.md).

**B189-1b material-publication checkpoint**: 2026-09-19 — the r5 frozen
material-backed publication diff received official read-only `STATIC_PASS`.
The affected DI closure and the newly registered `spec189-canonical-publisher`
target built with root Waf using `-j4`; the focused material-backed publication
assertion passed 1/1, related publisher regressions passed 5/5, and the
protected request, placement, material, provider-stage and CLI C++ selectors
passed. The existing full publisher suite still has four fixture/environment
failures, which are recorded separately and are not counted as a product PASS.
The previous real r38 run remains stopped at
`PREPARATION_FAILED / DI_NATIVE_PUBLICATION_MATERIAL_LIMIT`; no MiniNDN or
Qwen qualification is claimed. See [material publication evidence](evidence/b189-material-publication-20260919.md).

**B189-1b r39 boundary**: 2026-09-19 — a fresh run using the repaired global
candidate crossed the former publication-byte rejection but stopped at
`PREPARATION_TIMEOUT / DI_NATIVE_PREPARATION_TIMEOUT` during real Qwen
preparation. Cleanup passed; no ACK, Selection, Provider assembly, execution,
terminal response or qualification was observed. T003 remains `PARTIAL`; the
next action is to diagnose and reduce preparation cost before another retry.

**Audit reconciliation checkpoint**: 2026-09-19 — 已将 [DI/Repo static audit](evidence/di-repo-design-static-audit-20260919.md) 与当前源码重新对账：F01 的 material-only consumer 已有局部生产接线和 C++ selector，但真实 protected ingress 仍未验收；F02 已有 focused C++ selector 但实际 ORT/RSS 与完整候选资格仍开放，F08 仍开放，F05/F09 已有 focused C++ selector 但完整候选边界仍开放；F03/F04/F06/F07 分别标为条件性或 legacy follow-up。新增 FR-027..FR-029、T003-R1..R3 与 T009-R1，未将任何任务勾选完成。[audit reconciliation](spec.md#audit-reconciliation--2026-09-19)

**Protected range-store checkpoint**: 2026-09-19 04:52 -05:00 — B189-1a 的冻结范围通过官方只读 `STATIC_PASS`；受影响的 NDNSF targets 用仓库 Waf `-j4` 完成 compile/link，Repo range-store 6/6 C++ cases 和生产 Runtime protected publication 1/1 C++ case 通过，requester `--help` 入口通过。证据记录了 Core ciphertext publication、Repo generation/lease fence、worker cancellation/rollback、bounded reads、source release 和第二次 prepare 无对象增长。[protected range-store evidence](evidence/b189-protected-range-store-20260919.md) 只关闭本地 protected publication 接缝；T003、真实 Qwen、ACK/Selection、Provider、MiniNDN/Tiger 仍为 `PARTIAL`/open。根 NDNSF Waf 只负责 NDNSF 自有目标；NAC-ABE 由其自身构建系统及已安装 SDK 负责（canonical CMake，legacy Waf deprecated），未递归构建。

**B189-2 production-ingress checkpoint**: 2026-09-19 — 在现有 global-r3 Waf tree 对 `integration-tests` 的受影响源以 `-j4` 增量编译成功（39.072s，峰值 RSS 2,114,188 kB，0 swap），并运行真实 C++ `Spec170NdnsfDiCoreFlow` 生产入口 selectors：双 Provider D2b 请求到最终响应、ACK 后 post-Selection runner preparation、篡改 capability 拒绝均通过；post-Selection preparation factory 在手工 ACK/Selection 发布前两个 Provider 都为 0，发布后达到 provider0=2/provider1=1。该组用例证明 production handler 位于真实 ACK/Selection 后路径，但仍未直接记录未选 Provider 的 source fetch/assembly 计数，也未使用真实 Qwen canonical manifest；因此只登记为 T005 的 `FOCUSED_CXX_PASS`，不关闭 T005/B189-2。详见 [production placement evidence](evidence/b189-placement-production-20260919.md)。

**B189-3/T007 rerun checkpoint**: 2026-09-19 — B189-3/T007 的冻结范围经官方只读 `STATIC_PASS` 后，使用正确的 `../waf` 入口在 `build-spec189-b189-3-global-r3` 以 `-j4` 完成六个受影响目标编译链接（16.228s）：`ndnsf-distributed-inference`、`spec185-provider-assembly`、两个 Spec189 oracle CLI/fixture target 和 material oracle。C++ material oracle 15/15、provider-stage oracle 18/18、CLI oracle 5/5 通过；真实 `Provider::serve` selector 通过并记录同一 `preparationId` 的 assembly/ready 配对。首次错误的 `waf` 路径调用已登记在 failure log，未计为产品失败。该出口仍保持 T007 `PARTIAL`：没有独立模型数值、特定 upstream endpoint/compute-start 因果、真实 Qwen 多 token、MiniNDN 或复用资格。[causal oracle evidence](evidence/b189-causal-oracle-20260919.md)

**F02 focused checkpoint**: 2026-09-19 — T003-R1 的 production `MemorySnapshot`/terminal
guard 通过 r2/r3 官方只读复审；仓库根 Waf tree 重新配置并以 `spec189-preparation-memory`
完成 compile/link，修复 build-tree 与 `/usr/local` 同 SONAME 混链后 selector 通过 1/1。
本证据只覆盖分类计数与 post-publication/pre-cache cancellation rollback；实际 ORT allocator/RSS、
真实 Qwen 和完整资格仍开放。[F02 evidence](evidence/b189-f02-memory-20260919.md)

**F09 focused checkpoint**: 2026-09-19 — T003-R3 的 FilesystemRepoStoreBackend
`fsync`/`close`/directory-`fsync` 反例已完成 r4 只读 `STATIC_PASS`、`-j4` 单目标构建和
3/3 C++ selector 运行；F09 仅为 `FOCUSED_CXX_PASS`，不关闭 T003，也不替代 F02/F05
或完整 publication/qualification。见 [F09 evidence](evidence/b189-f09-fd-owner-20260919.md)。

**F05 focused checkpoint**: 2026-09-19 — T003-R2 的 RepoCore mixed quota
selector 完成 r2 只读 `STATIC_PASS`、`-j4` 单目标构建和 2/2 C++ cases；range
reservation 现在同时约束普通 vector 与 exact Data packet admission，abort 后可恢复。
这只关闭 F05 的 focused selector，不关闭 replacement/失败回滚、T003 或 protected
publication/qualification。见 [F05 evidence](evidence/b189-f05-quota-20260919.md)。

**Causal oracle checkpoint**: 2026-09-19 01:37 -05:00 — T007 多次 runner preparation 独立日志 ID 与逐 ID 判据、正常 CLI 的材料/范围/末段 terminal 门已通过 r2 只读任务与组合审查；增量构建 2m54.872s，C++ 材料15例/阶段18例/CLI 5例通过，真实 Provider::serve 接线 selector 通过并记录配对 preparationId。七个输入匹配审查快照；独立模型输出、endpoint 因果、真实 MiniNDN/复用仍未完成，保持 PARTIAL。[causal oracle evidence](evidence/b189-causal-oracle-20260919.md)

**Material oracle checkpoint**: 2026-09-19 01:25 -05:00 — T007 原子材料事件判据已通过只读任务/组合静态门，两个 oracle target 增量构建 23.813s，C++ parser 15 cases PASS；已核对审查/构建源码身份。共享 checker/test 独立归档，CLI 混合改动保留待整合；因果顺序、独立输出、同 handle 复用及真实 MiniNDN 仍未完成。[material oracle evidence](evidence/b189-material-oracle-20260919.md)

**DI/Repo repair design analysis**: 2026-09-19 — 已复核并行新增的 material-only consumer，上一轮 F01 的“未接入”不再代表最新源码；局部 consumer PASS 与真实生产链资格仍区分。完成 [repair design analysis](evidence/di-repo-repair-design-analysis-20260919.md)，建议统一材料读取、存储提交/目录恢复、预算与 turn 所有权，并明确小修复和后续结构收敛的边界。仅分析，未改产品源码、未构建或运行实验、未修改任务完成状态；建议为 `PROPOSED`，不覆盖冻结目标。

**Requester Repo checkpoint**: 2026-09-19 01:23 -05:00 — T003 source owner 首次 external initializer 重复读取已修复，r6 复审及增量构建通过；Repo C++ 5/5，私有 spool 下 Runtime production-entry 1/1。DI 全局安装已完成，DI/Core build/global SHA256 一致，requester 的全局动态链接路径已核对；两个调用方的混合改动尚未归档。真实跨节点读取/Qwen/MiniNDN 保持 PARTIAL。受保护 publication 继续走 Core ServiceUser；详见 [requester Repo evidence](evidence/b189-requester-repo-20260919.md)。

**DI/Repo design audit checkpoint**: 2026-09-19 — 原审计是 13b79ad1 工作树时点的只读扫描，覆盖 372 个源码文件清单、173 个 Python AST 和准备/发布/组装、Repo 目录/容量/持久化、Conversation owner。后续 material-only consumer 已接入局部生产组装路径并通过 C++ selector，因此 F01 的“未接入”只保留为历史边界；准备峰值、混合写入预算、turn owner 和 fd 错误路径仍开放。F03/F04/F06/F07 不属于当前 native protected qualification 的默认调用方，保留条件性/legacy 状态。报告不是逐行全量审查或产品验收，保持 `PARTIAL / NOT_STATIC_PASS`；详见 [DI/Repo design static audit](evidence/di-repo-design-static-audit-20260919.md) 和 [repair design analysis](evidence/di-repo-repair-design-analysis-20260919.md)。

**Updated**: 2026-09-18 19:45 -0500 — the file-backed ONNX assembly/resource subunit passed the r13 read-only static gate and focused validation (`22 passed, 1 skipped`); the real Qwen canonical identity scan passed with 3,689,700 kB peak RSS and zero swaps. The audit retired T008 as a standalone capability task: guard/lifecycle checks are cross-cutting gates owned by T003/T006 and closed by T009. The new single-target global install helper passed static review, its preflight and flags regression suite passed (`100 passed`), and `ndnsf-distributed-inference` built/installed in 11.899s with matching build/global SHA-256 and global ONNX Runtime linkage; no MiniNDN qualification is claimed. Protected Provider ingress, native Repo publication, ACK/Selection, two-provider execution and full cleanup remain open. See [B189 convergence evidence](evidence/b189-convergence.md), [target install evidence](evidence/b189-global-target-helper-20260918.md), [resource evidence](evidence/b189-resource.md), and [failure log](../../docs/failure-log.md).
**Latest checkpoint**: 2026-09-19 — the strict native ONNX source-reuse fix received read-only `STATIC_PASS`; the affected DI/worker/unit targets built in `1m52.201s` with `-j2`, and the five focused ONNX selectors passed after building their worker tools (`11/11`, `5/5`, `9/9`, `11/11`, `30/30`). The combined selector was stopped at a host resource boundary. Real Qwen runs r30 through r33 all stopped at `RESOURCE_BOUNDARY:swapIo` before an interpretable two-provider workload result; MiniNDN/workload remain `NOT_EVALUATED`. r32 used the correct global profile after a separate stale-profile preflight rejection; r33 reached the running/drained sampling phases but still did not start MiniNDN. See [ONNX identity/resource evidence](evidence/b189-onnx-identity-resource-20260919.md).
**Runner-preparation checkpoint**: 2026-09-19 — T006 generation/position metadata binding passed read-only `STATIC_PASS`; global-r3 `unit-tests` rebuilt with `-j1` in `28.600s`, and `NativePreparationContext*` passed 3/3. The regression covers stale adapter metadata removal and authenticated successor/position write-back. This is a metadata unit boundary only; real Provider ingress and ORT execution remain open. See [runner preparation evidence](evidence/b189-runner-preparation-20260919.md).
**Materialization-worker checkpoint**: 2026-09-19 — T006 canonical source/initializer ownership and bounded worker framing passed final read-only `STATIC_PASS` after two review fixes. The affected DI/worker/unit build completed with `-j1`; focused C++ selectors passed `5/5`, `3/3`, `4/4`, and `30/30` (the last with an explicit worker binary directory). This remains a component boundary; protected Repo ingress, real two-Provider execution, output oracle, and drain are open. See [materialization worker evidence](evidence/b189-materialization-worker-20260919.md).
**ONNX memory checkpoint**: 2026-09-19 — the direct-vector source reader and selective ONNX identity/shape materialization passed read-only `STATIC_PASS`. The global-r3 DI target installed successfully with `-j1` in `7m17.389s`; the unit-test target rebuilt in `5m56.704s`; `NativePreparationContext*` passed 3/3, the combined ONNX/assembly selector passed 18 cases, `Spec189*` passed 4 cases, and the selective shape regression passed. Qwen r36 still stopped after both Providers became ready at `RESOURCE_BOUNDARY:MemAvailable` (minimum `1557188608` vs floor `1610612736`, zero swap I/O, peak RSS `4591411200`); no workload or qualification result exists. See [ONNX memory/r36 evidence](evidence/b189-onnx-memory-r36-20260919.md) and [failure log](../../docs/failure-log.md).
**Protected Runtime publication checkpoint**: 2026-09-19 — the new C++ selector passed read-only `STATIC_PASS`; after fixing the fixture spool and bounded Repo read oracle, `spec189-prepared-request` rebuilt from global-r3 with `-j1` in `29.085s`, and the two `Spec189*` cases passed from the repository root. The protected case used Runtime's default Core publisher with `RepoEncryptedLargeDataStore`, verified encrypted source/root/material manifests, per-payload receipt bindings, bounded material reads, source release and no second-prepare object growth. This closes only a local protected-publication boundary; ACK/Selection, Provider assembly, real Qwen execution, output oracle, resource drain and MiniNDN remain open. See [protected Runtime evidence](evidence/b189-protected-runtime-20260919.md) and [failure log](../../docs/failure-log.md).
**Material consumer checkpoint**: 2026-09-19 — the T006 material-only consumer passed final read-only `STATIC_PASS` in r6. The global-r3 `integration-tests` target rebuilt with `-j1` in `3m37.037s`; the C++ selector passed its material-only positive path and aggregate-budget negative path, and the complete `Spec175NativeAssembly` suite passed 8/8 from the repository root. The consumer now reads only the authenticated manifest and selected payloads after Selection, with no source/initializer fallback and a pre-fetch aggregate budget. Production Core ACK/Selection ingress, real Qwen two-Provider execution, output oracle, drain and MiniNDN remain open. See [material consumer evidence](evidence/b189-material-consumer-20260919.md) and [failure log](../../docs/failure-log.md).
**Baseline**: `3e53fec5` plus pre-existing implementation and unvalidated protected-store draft; not a clean qualified candidate.

B189-1a publisher weak-pin、Repo identity fence 和 Core worker/cancel/key release
均已获官方只读 `STATIC_PASS`；组合构建和 package/cache owner selectors 已通过。
剩余的是 protected Repo source owner 在真实 Qwen prepare 中的接线和 B189-1b 原子
材料 consumer，详见 [prepare evidence](evidence/b189-prepare.md)。

T003 源借用修复已静态通过；增量构建 56.712s。初次 native heap corruption 已定位为
installed DI 的旧 ABI（publication 304 vs 360 bytes），全局安装同步后两个 C++ selectors
连续三轮通过。真实受保护 Repo 接线与原子材料仍未完成。见 [prepare evidence](evidence/b189-prepare.md)。

尚无 `QWEN_TWO_PROVIDER_PASS`。r25 run-record 仍 FAIL；Provider 日志已出现
`EXECUTION_ENTERED` / `ASSEMBLY_STARTED`，不能继续称“执行入口完全未观察到”。
尚未证明 runner ready、两段执行、有效终态及资源回收闭合。stream gap 是症状，不能单独认定根因。

本轮 B189-1a 组合构建 342 tasks / 7m8.097s，package-owner selector 3 次、Repo
protected selectors 6 次、bounded publisher 2 次及 Runtime prepare 1 次均 PASS；
保留全局依赖与定向构建、Repo 冷热发布/事务/层 payload fixture、同一 PreparedModel
两请求的 publication-counter selector、placement/cache selector、已注册 C++ 日志 oracle。
它们是组件证据，不能拼成真实 Qwen 全链 PASS。
requester 已有 encrypted range-store 注入草稿，尚未静态/构建/运行验收；
不能注入 plain Repo publisher 替代 protected publication。旧 assembler 仍获取完整 initializer；
分层 producer/consumer 尚未闭合。日志 oracle 的严格事件总序也需修正。

本轮审计见 [audit correction](evidence/spec189-static-audit-20260918.md#architecture-and-progress-correction)。
旧详细 checkpoint 保留在上述 Git 基线和原批次证据；本表取代十任务线性调度。
T001 映射已关闭；host direct/launcher guard 已复审，39 host checks 通过；
两个 C++ 目标 -j4 构建成功，4 个 lifecycle 用例三轮通过。真实路径 native counters
及完整资源回收仍待验证；资源门由 T009 收口，未运行完整模型。磁盘现约 34 GiB 可用。
文档修订已获冻结 v2 的 DOCUMENTATION_STATIC_PASS；11/11 技能入口、6 task ID、
29 FR、链接/锚点及 diff 检查通过，详情见上述审计记录。产品验收保持 PARTIAL。

## Execution Progress

6 项是能力任务，不按数量计算产品百分比。T003 三个独立执行出口见
[bounded execution units](batch-execution.md#bounded-execution-units)，B189-1a 已关闭，
当前下一步为 B189-1b。
本轮下一步仍为 T006-R2：先完成 r98 `ownedSwap` 资源边界的单变量诊断，核对
requester/Provider 的进程级工作集与宿主 swap 基线；在保持完整身份契约和维护资源门的
前提下，用全新安装候选运行真实 MiniNDN 请求。只有越过资源门并观察到 ACK/Selection
后，才继续处理材料、runner、handoff 或 terminal 边界。T006-R1 的 admission→ROOT
单调序列已是 focused C++ 出口，不再重复领取。
每个出口验证后立即记录，不等整项 T003 写完才第一次构建。
本轮新增 memory-lifecycle audit 已关闭两个局部 working-set 出口：prepare 的完整
source/initializer 在 material manifest 固定后可释放，Provider role model 物化后可
释放 selected material；取消/rollback 反例也已用 C++ 屏障和 drain 通过。DI/selector
增量构建通过，定向 selector 全部通过；完整 selector 仅剩 real-Core fixture 的 `/tmp`
staging `Permission denied`，因此不改变 T003/T006/T009 的 PARTIAL 状态，也不把环境失败
记为产品 PASS。[memory audit](evidence/b189-memory-lifecycle-static-20260920.md)
host guard 和小型 lifecycle safety entry 已达到其前置出口；剩余 native counter
接入属于 T003/T006 的实际 owner，最终在 T009 以完整采样和 drain 证据收口，不再
创建重复的资源行政批次。
本轮文档 v2 已获 DOCUMENTATION_STATIC_PASS；结构/29 FR/链接与技能同步检查通过，
见 [follow-up verification](evidence/spec189-static-audit-20260918.md#follow-up-verification)。
受保护接缝已通过 B189-1a 静态门、受影响目标构建及 C++ focused selectors；Repo
adapter producer/consumer 和 Runtime protected publication 的本地出口已记录，
但 protected Provider consumer/真实 Qwen 链路仍未审查，本轮未构建或运行模型。

| Unit / Details | Status | Depends | Remaining exit / Evidence |
| --- | --- | --- | --- |
| [T001 Freeze integration boundary](#t001) | DONE | — | 2026-09-18 13:29 -0500：真实接线/候选/后继缺口及五 lane 映射已只读审查；仅关闭实施边界。[convergence](evidence/b189-convergence.md) |
| [T003 Prepare and reuse Repo materials](#t003) | PARTIAL | T001 | material-backed publication budget/receipt path and focused C++ selector now pass; protected range-store/material-only consumer remain local boundaries. F02 actual ORT/RSS, F05 replacement/rollback beyond focused selector, F09 complete publication boundary, real Qwen source release and protected ingress remain open. [material publication](evidence/b189-material-publication-20260919.md); [prepare](evidence/b189-prepare.md); [protected range-store](evidence/b189-protected-range-store-20260919.md) |
| [T005 Authenticate placement](#t005) | PARTIAL | T003 | ACK 后规划、signed Selection、生产 ingress handler/no-fetch 计数。[placement](evidence/b189-placement.md); [production ingress](evidence/b189-placement-production-20260919.md) |
| [T006 Materialize selected ranges](#t006) | PARTIAL | T005 | material-only C++ consumer、aggregate budget、Selection-scoped admission sequence 与 assembly suite 已通过；r70 已进入 Provider 真实材料读取，但 stream consumer 仍未跨 selected Provider progress 续命，生产 assembly、owner/cancel counters 仍待完成。[material consumer](evidence/b189-material-consumer-20260919.md); [admission/r58 boundary](evidence/b189-admission-sequence-r58-20260919.md); [r70 boundary](evidence/b189-native-r70-progressed-stream-boundary-20260919.md) |
| [T007 Validate handoff and output](#t007) | PARTIAL | T006 static gate | 阶段/材料/CLI 门和持久 admission sequence 已验；r70 尚无 runner、NDN endpoint 因果、hidden-state handoff、独立输出与真实多 token 仍待验。[causal oracle](evidence/b189-causal-oracle-20260919.md); [r70 boundary](evidence/b189-native-r70-progressed-stream-boundary-20260919.md) |
| [T009 Qualify reuse and repeat](#t009) | PARTIAL | T003 + T005 + T006 + T007 | r70 resource guard/cleanup passed but stopped during post-Selection assembly; same-handle two requests, independent repeat, F08 generation-guard, native counters, terminal output and qualification remain open。[convergence](evidence/b189-convergence.md); [r70 boundary](evidence/b189-native-r70-progressed-stream-boundary-20260919.md) |

## Task checklist

- [x] T001 [US1] Freeze the remaining production integration and candidate boundary.
- [ ] T003 [US1] Connect topology-independent Qwen preparation, Repo publication and reference-only reuse.
- [ ] T005 [US2] Verify real ACK-driven planning and authenticated Selection at production ingress.
- [ ] T006 [US4] Fetch selected Repo materials, assemble bounded native CPU runners, and expose native resource counters.
- [ ] T007 [US3] Validate real handoff, causal events and independent terminal output.
- [ ] T009 [US5] Qualify the resource envelope, complete MiniNDN path, same-handle reuse and independent repeat.

## Audit follow-up registry

这些是现有能力任务下的稳定子出口，不是新的行政任务，也不改变 Spec189 的六项
能力任务计数。每项都必须有 C++ production target/selector 和独立失败边界；未完成
时保持所属 T 项 `PARTIAL`。

| Subtask | Finding | Owner / dependency | Status | Exit evidence |
| --- | --- | --- | --- | --- |
| T003-R1 | F02 preparation peak | Runtime/ONNX preparation owner; before T009 full model | FOCUSED_CXX_PASS / QUALIFICATION_OPEN | C++ selector records source/material/encryption/ORT budget categories and post-publication cancel/retry cleanup; actual ORT allocator/RSS and real Qwen remain open; [F02 evidence](evidence/b189-f02-memory-20260919.md) |
| T003-R2 | F05 mixed quota reservation | RepoCore range/vector/Data packet admission; before protected candidate | FOCUSED_CXX_PASS | C++ mixed range/vector/Data selector passed; replacement/failure rollback and protected candidate remain open; [F05 evidence](evidence/b189-f05-quota-20260919.md) |
| T003-R3 | F09 fd error ownership | FilesystemRepoStoreBackend error path; before protected candidate | FOCUSED_CXX_PASS | injected fsync/close failure, one-owner/no-duplicate-close and preserved manifest boundary; [F09 evidence](evidence/b189-f09-fd-owner-20260919.md) |
| T006-R1 | executable assembly progress sequence | NativeProvider executable runner factory; before the next real Qwen retry | FOCUSED_CXX_PASS / QUALIFICATION_OPEN | executable uses the Selection-scoped sequence shared with admission, and a C++ reporter-contract regression uses two independent reporters with admission sequence 1 followed by ROOT sequence 2; no real MiniNDN credit until the fresh run crosses the boundary; [T006-R1 evidence](evidence/b189-t006-r1-progress-sequence-20260919.md) |
| T006-R2 | cross-provider stream progress binding | Core `ServiceUser`/`InvocationStream` collaboration consumer; before the next real Qwen retry | FOCUSED_CXX_PASS / QUALIFICATION_OPEN | consumer allowlists exact `{provider, providerSelectionDigest, role operationId}` tuples and monotonic freshness; local lifecycle/status selectors pass, but r70 remains the old-candidate boundary and a fresh installed-candidate run is required; [progress heartbeat evidence](evidence/b189-r4-progress-heartbeat-20260919.md); [r70 boundary](evidence/b189-native-r70-progressed-stream-boundary-20260919.md) |
| T009-R1 | F08 turn owner race | Conversation/PreparedModel handle installation; after T007 and before same-handle PASS | PLANNED | C++ barrier interleaving where older terminal/exception cannot overwrite or close newer turn |
| F03/F04 follow-up | catalog snapshot/history gap | Only if catalog snapshot/delta becomes a candidate caller | DEFERRED | snapshot-required + incarnation/oldest-sequence C++/Python sync evidence |
| F06/F07 follow-up | compatibility replacement/durability | Legacy helper/backend maintenance, not current native protected path | DEFERRED | separate compatibility task and failure model; no Spec189 PASS credit |

## Retired task IDs

合并不代表完成，旧 ID 不再单独领取或勾选为 PASS。

| Former ID | Disposition | Preserved obligation |
| --- | --- | --- |
| T002 | MERGED_INTO T003 | Qwen graph/config/digest 验证、原子层/shared 材料生成、staging cleanup |
| T004 | MERGED_INTO T003 | reference-only、两请求无新增发布、stale/released/oversized negatives |
| T010 | MERGED_INTO T009 | 独立重复、候选一致性、最终 verdict 与文档交付 |
| T008 | MERGED_INTO T009 | host guard、受控 stop、native resident/runner counters、完整采样与 drain；小型 guard/lifecycle 证据保留在 resource record |

<a id="t001"></a>
## T001 — Freeze integration boundary

**Read**: 最新 failure-log/raw boundary、架构阅读集、当前 Spec、global dependency receipt。
**Write**: `evidence/b189-convergence.md`；不另建 gate 框架。

1. 用 CodeGraph 固定 Runtime::prepare→RepoSourceProvider→DI_NativeRequester→
   NativeCanonicalOnnxAssembler→NativeProviderHandler/NativeEpochCoordinator 的实际接线与缺口。
2. 标明生产 CLI、独立 C++ assertion target、实际 Waf target/源码闭包；
   核对已有全局 ABI/receipt，只有缺失/改变/不兼容才安装或重建。
3. 固定 model revision、动态 KV schema、服务角色/key registry、拓扑、资源阈值、
   摘要派生入口和 handoff endpoint/attempt/sequence。最终 binary hash 在批末更新，
   不要求尚未实现的验收先通过。

**Acceptance**: 五 lane 的已验/待改/待测映射可执行，无循环依赖；
不因文档修改/run-id 改变重新全量构建，不授予产品 PASS。

## Retired T008 — cross-cutting gate

T008 不再作为独立能力任务。它保留的义务由实际 owner 承担：T003/T006 在各自
selector 中交付 native resident/materialization/lease/runner counters 和取消回收
反例，T009 在每次 full-model run 前调用现有 host guard，并在成功或分类停止后
统一检查采样、child 状态和 drain。`evidence/b189-resource.md` 保留已有 39 个
host checks、lifecycle fixture 和真实 identity 记录；这些证据不单独授予产品 PASS。

资源门规则：阈值来自 immutable profile，超过阈值必须分类为 `RESOURCE_BOUNDARY`；
只清理本 run staging，保留原始日志和有效 Repo 对象证据；`finally`/`kill` 本身
不等于 lifecycle PASS。由于这部分不新增生产能力，不再为它单独建批次或重复编译。

<a id="t003"></a>
## T003 — Prepare and reuse Repo materials

**Write**: `Runtime.cpp`、`NativeCanonicalPreparationCatalog.*`、
`NativeCanonicalArtifactPublisher.*`、定义 NativeCanonicalSource 的
`NDNSF-DistributedInference/cpp/adapters/onnx/NativeOnnxRecipeAssembler.hpp`、
`NDNSF-DistributedRepo/include/ndnsf-distributed-repo/RepoSourceProvider.hpp`、
`examples/DI_NativeRequester.cpp`；已有 Repo/PreparedModel C++ selectors；
`evidence/b189-prepare.md`。按实际定义路径修改，不复制 Qwen API。

共享 protected 接缝另涉及 `ndn-service-framework/ServiceUser.{hpp,cpp}`、
`EncryptedLargeDataRangeStore.hpp` 和 Repo `RepoEncryptedLargeDataStore.hpp`。
先按 B189-1a 验证 protected Repo 接缝，再按 B189-1b 实现原子材料；Core 不依赖
DI/Repo 类型。T003 不负责 ACK/Selection ingress，该边界属于 T005。
`NativeCanonicalArtifactPublisher::CacheState::prepared` 当前只保留 receipt 和 weak
serving pins；`PreparedModelPackage`/活动 request 才是强 owner。复用
ModelPreparationCache 预算/淘汰作为唯一保留策略，publisher 不成为第二个无界强 owner，
活动 package/request 仍保证可读。B189-1a 只验证受保护接缝和真实 package/cache owner
反例；B189-1b 才冻结原子材料 schema，不把未来 schema 当作当前 API。
C++ 反例必须走真实 publisher→package→淘汰路径，不能只手动 reset Core token。
B189-1a 当前 worker/cancel/key 与 Repo identity 静态门、组合构建和受保护
package/cache owner runtime selector 已通过；这只关闭本地 protected publication
接缝，不能把它写成 T003 完成。
具体出口与反例见 [bounded commit](contracts/model-preparation.md#bounded-commit-and-identity-ownership)。

**Audit follow-up exit B189-1c**：T003-R1 记录 source/initializer/material/encryption/ORT
各类 peak owner 与取消/重试清理；T003-R2 让 range reservation、普通 vector/Data packet
写入和 replacement 共用逻辑 quota；T003-R3 对 fsync/close failure 验证单一 fd owner。
三个出口必须使用 C++ production selector，分别记录首个失败边界，不能由 Python
脚本或磁盘剩余空间检查替代。

1. pinned canonical graph/initializer 生成拓扑无关原子层与 shared tensor 引用。
   层→节点/权重范围由 graph 推导并校验；embedding/final/tied weights 按内容去重。
   不能把预导出的 [0,14)/[14,28) 最终模型当 prepare 格式。
2. 复用 Repo manifest/payload owner/文件后端，最小版本化扩展 layer→graph/tensor
   object 或受验证 byte-range 的 digest/size/依赖关系。读单元有界，
   发布不再累积整份 vector；旧 schema 明确拒绝或走受测兼容路径，不能静默降级。
3. 将实际 requester 接入 Runtime Repo publisher/source 生命周期；对象持久化、
   manifest commit 且正常 Repo 读取可达后才 READY。source owner 可释放，
   服务与活动 lease 存活；不可达/对象丢失明确失败，request 不隐式重发模型。
4. 同一 immutable identity 的重复 prepare 命中已有 manifest/reference，且不会
   固定 Provider placement；同 handle 的两次请求和 publication/ingest 零增量由
   T009 的真实 MiniNDN 资格统一证明，避免在 prepare selector 中重复模拟最终链路。
5. 复用已通过 selector，仅补 real-Qwen receipt、源释放、stale manifest、
   digest/range/schema、staging rollback 和必要 envelope negatives。
   离线 snapshot→canonical 导出可复用，但不替代 native prepare/Repo。

**Acceptance**: C++ production entry 验证真实 Repo 材料、commit/可达性、源释放和
immutable prepare lookup。完整 Qwen run 由 T009 先通过资源门；此处不要求两
Provider 执行成功，也不重复证明最终同 handle 两请求。F02/F05/F09 的 B189-1c
出口未通过前，不能把完整候选标为 ready。

<a id="t005"></a>
## T005 — Authenticate placement

**Write**: NativeRequestPreparation/Envelope、必要 Core/NativeProviderHandler 接线、
`tests/integration-tests/spec189-placement-oracle.t.cpp`；
`evidence/b189-placement.md`。

1. 真实 ACK offers 后由 planner 决定两个覆盖范围；profile 可约束 [0,14)/[14,28)，
   不能改变 prepare manifest 或注入假 ACK/Selection。
2. 使用已有 typed projection/签名/canonical grant identity builder，
   绑定 manifest、Provider、role/range、attempt/epoch、plan digest。
3. 生产 Selection ingress 验证无 offer、stale epoch/digest、未选 Provider、
   overlap/out-of-range；实际 fetch/runner factory 计数为零。
4. 修正现有 selector 的 CPU backend/ABI 和 synthetic grant fixture；
   保留其组件价值，禁止用 cache-layer hit 证明网络授权已验。

**Acceptance**: 真实 signed path 与生产 ingress C++ assertions 通过；
无效选择无重型副作用，不依赖 T006 模型执行形成循环。

<a id="t006"></a>
## T006 — Materialize selected ranges

**Write**: NativeCanonicalOnnxAssembler、NativeOnnxAssemblyWorker、provider/Repo adapter、
已有 assembly/ownership selectors；`evidence/b189-execution.md`。

1. 实际 consumer 使用 T003 同一 manifest/receipt，授权后读取选定层与显式 shared
   tensors；替代整 initializer 下载再切片，未接线不能隐式回退旧路径。
2. 有界读取/文件物化与 RAII lease；digest/size/range/role/manifest 验证后才进 ORT。
   记录实际 fetched bytes、resident buffers、mapped files、runner owner，
   不能只用 cache.stats 或 RSS 代替源对象释放证明。
3. 错包/取消/组装失败测试无半成品 runner、文件/lease/worker 泄漏；
   保留已验 endpoint-preservation 回归，不重新实现。
4. 不可变内容可缓存复用，每请求重验授权；KV/会话与 runner 生命周期分离，
   不强制 warm request 重建 runner，不长期持有整源。

**Acceptance**: 实际 provider factory 用 Repo 选中材料构造 CPU runner，
没有每 Provider 整模型临时副本；共享 bytes 与失败/回收出口可核对。

<a id="t007"></a>
## T007 — Validate handoff and output

**Write**: 必要 NativeProviderHandler/NativeEpochCoordinator 修复、
`examples/Spec189TwoProviderOracle.cpp` 与已有 C++ handoff fixtures；
`evidence/b189-execution.md`。

1. 真实 Core/NDN hidden-state handoff 核对 endpoint digest、attempt/model/role/
   sequence、shape/dtype；不直接跨节点传内存对象。
2. 按 [placement contract](contracts/placement.md) 修正事件 checker：
   model assembly 与 upstream fetch 可交错，首段没有 upstream dependency；
   authorization/runner/input 均就绪才 execute，末段响应后所有 owner drain。
3. 覆盖成功、cache-hit、缺事件、错误因果/identity 的最小 C++ fixture。
   生产 CLI 和日志 checker 均不单独证明模型正确。
4. 冻结短输入及独立 reference，以 C++ 检查 shape/finite、
   冻结 logit tolerance 或 top-token 期望；digest 只作身份。
   保留 cancel/provider stop/stale handoff 反例，不扩张质量或 KV 性能工程。

**Acceptance**: 原生 handoff 与独立输出判据可执行，checker 不误拒合法次序且拒绝异常；
最终真实 Qwen 资格仍由 T009 负责。B189-3 的 assembly-admission progress 已有稳定
C++ 出口；在下一次真实候选运行前，不再加入新的组件职责或仅为减少重试而调整超时。

<a id="t009"></a>
## T009 [US4, US5] — Qualify resource envelope, reuse and repeat

**Write**: 维护的 MiniNDN runner、单进程 C++ requester/driver 复用入口及证据 checker；
raw logs 放唯一 `.codex-tmp` run 目录；`evidence/b189-convergence.md`；
失败同步 failure-log。

1. 在 T003/T005/T006/T007 runtime 出口通过后，冻结实际源码内容、global ABI、模型/
   manifest、profile/topology、binaries/oracle；自动派生摘要，检查 policy/key/module/disk。
2. 两 CPU Provider，native prepare→Repo→ACK→planner/Selection→按需组装→
   NDN handoff→有效输出→drain。必须由一个长期存活的 C++ requester/driver
   在同一 Runtime/PreparedModel handle 上先 prepare 一次，再提交两个独立
   request；不能用脚本启动两个独立 requester 进程来替代。publication 增量为零，
   两个结果均通过独立 C++ oracle。
3. 原始证据落盘后保持同 candidate，用新 run-id 重复上述场景；
   request id/key/临时路径属于 run identity，不使 candidate 摘要变化。
4. 每次运行先通过 host guard，再比较 fetched bytes/cache/runner、RSS/Repo resident/
   物化峰值和 post-drain baseline。
   warm cache 可复用 runner；不能为凑计数而强制重建。
5. 失败保留第一已证实边界/未知部分，修复复审受影响范围再复测。T007 必须先
   提供独立固定输入的 C++ numerical/output oracle；token schema、digest 或
   CLI 日志不能替代它。
   实现引起的 API/行为变化按 Design/MANAGEMENT.md 同步契约/PDF/文档交付。
6. T009-R1 用 C++ 屏障控制旧 turn terminal/exception、新 turn start 和 handle
   installation；generation 不匹配时旧 turn 不得覆盖 active owner 或被 close 取消。
   该门通过前不能把“同 handle 两请求”仅凭两个结果文件认定为复用 PASS。

**Acceptance**: 两独立运行均成功，且各自同 handle 两请求/输出/资源/drain 齐全，
才 `QWEN_TWO_PROVIDER_PASS` / [x]。classified failure 不算完成。无 SIF/Tiger/27B 工作。

## Logical Batches and Dependencies

### Current Checkpoint — r140 focused secure-erase candidate

The v4 immutable source snapshot passed read-only `STATIC_PASS` after the
protected-source erase, lease-ordering, and range-backed material repairs. The
affected native closure then built `556/556` with Waf `-j4`; focused C++
selectors passed material `2/2`, canonical publisher `14/14`, ONNX activation
`9/9`, and protected-directory cleanup `1/1`. Six affected targets were
installed and their dependency closure was checked with `ldd`. The first
publisher selector failure against the default `/tmp` staging directory was a
permission boundary; the rerun with a private `0700` staging directory passed.
Evidence: [r140 focused build and secure-erase](evidence/b189-r140-focused-build-secure-erase-20260920.md).
This is not a MiniNDN/Qwen or qualification result; T003, T005, T006, T007,
and T009 remain `PARTIAL`.

The subsequent real installed MiniNDN run `two-provider-global-r140` passed
the host resource gate, ACK closure, Selection commit, both Provider
Selection acceptance records, and both `GRANT_VERIFICATION` boundaries. It
reached Provider-0 assembly start and Provider-1 dependency fetch, then
Provider-1 exhausted 356 exact signed-data attempts with `error=deadline`
before Provider-0 published the requested tensor manifest. The run was then
operator-stopped while Provider-0 was still materializing; supervisor cleanup
passed. This is a classified upstream-readiness/deadline boundary, not a
resource, authorization, Repo, ORT, numerical-output, or qualification PASS.
Evidence: [r140 MiniNDN exact-fetch boundary](evidence/b189-r140-minindn-exact-fetch-boundary-20260920.md).
T003, T005, T006, T007, and T009 remain `PARTIAL`; the next repair must
review the initial dependency deadline/progress contract and use a new run ID.

### Current Checkpoint — r141 initial producer-readiness repair

The r140 boundary was reduced to a precise C++ regression: a consumer started
the first exact V3 manifest fetch before the producer had published that
manifest, while the existing fetch budget also applied the post-publication
`noProgressDeadlineMs`. The new
`V3DependencyIoWaitsForInitialProducerReadiness` integration case first failed
against the old implementation after the delayed producer publication
(`critical check published has failed`, selector rc `201`). The production
repair now bounds that first manifest fetch by the request hard deadline and
dependency fetch budget; subsequent segment fetches retain the
`noProgressDeadlineMs` bound.

The affected `integration-tests` target rebuilt `127/127`. The new regression
then passed, and the existing V3 manifest/segment case plus the new case passed
as a two-case selector with `No errors detected`. Raw build and selector output
is retained under
`.codex-tmp/spec189-r141-initial-readiness-regression/`; the immutable static
review snapshot is under
`.codex-tmp/spec189-r141-initial-readiness-regression/static-review-20260920/`.
This is a focused C++ repair only: no new installed candidate or real r141
MiniNDN run has yet observed manifest publication, runner readiness, terminal
output, numerical oracle, repeat, or qualification. T003, T005, T006, T007,
and T009 remain `PARTIAL`.
The frozen source review returned `STATIC_PASS` with no blocker. Its P2
follow-ups remain unobserved: an unpublished manifest must terminate at the
hard deadline/fetch budget, a smaller fetch budget must cap initial readiness,
and a post-manifest segment stall must retain the no-progress failure. These
are coverage improvements, not a qualification result.

### Current Checkpoint — r142 owned-swap resource boundary

The fresh installed-binary MiniNDN run `two-provider-global-r142` passed the
host gate at startup and reached both Provider `READY` states, signed ACK
offers, selection-assignment publication, and both `GRANT_VERIFICATION`
records at `BEFORE_ASSEMBLY`. Provider-0 also created an active assembly
staging root. Before any observable `EXECUTION_ENTERED`, `DEPENDENCY_FETCH`,
`RUNNER_READY`, terminal output, or numerical oracle, the maintained host
guard stopped the run at `RESOURCE_BOUNDARY:ownedSwap` after its owned-swap
limit was exceeded. Supervisor cleanup was `PASS`; requester cancellation and
Provider-1 socket EOF are shutdown consequences. The raw run and resource
trace are retained in [r142 evidence](evidence/b189-r142-owned-swap-boundary-20260920.md).

This is a host resource boundary, not a protocol/Repo/ORT/model result or
qualification PASS. T003, T005, T006, T007, and T009 remain `PARTIAL`. The
next real run requires a new run ID and a host state that stays below the
owned-swap guard before retrying the post-Selection path.

### Current Checkpoint — r143 MiniNDN routing-entry review and owned-swap boundary

The fresh r143 installed-binary run reached both Provider `READY`, signed ACK
offers, and `GRANT_VERIFICATION` at `BEFORE_ASSEMBLY`, then the unchanged host
guard stopped it at `RESOURCE_BOUNDARY:ownedSwap` with
`ownedSwapBytes=268681216` against `268435456`. Cleanup passed. No Selection,
execution, runner, terminal response, numerical oracle, repeat, or qualification
result exists; T003, T005, T006, T007, and T009 remain `PARTIAL`. Evidence is
in [r143 routing and owned-swap evidence](evidence/b189-r143-routing-plan-owned-swap-20260920.md).

The MiniNDN entry was then reviewed against the upstream NLSR and static-routing
examples. Because Spec189 requires deterministic application-prefix routes, the
entry now uses `Nfd + NdnRoutingHelper` only, removes the duplicate NLSR owner,
retains `--nlsr-wait-s` as a compatibility alias for `--routing-wait-s`, and
writes a validated node-to-APP plan. `py_compile`, `--help`, the node-plan
helper, and `git diff --check` passed. A five-node MiniNDN smoke then confirmed
that the static route `/spec189/smoke` published by `memphis` is visible in
`neu`'s FIB and that cleanup passes. This corrected entry has not yet been used
for a new real Qwen run. The read-only review-agent
`01a0c116-f576-76c3-bc5e-d322e2d4ae59` reviewed base `4ff5b700` to checkpoint
`c03260d2`, found no P0/P1/P2 issue, and returned `STATIC_PASS`; only optional
P3 dependency/FIB diagnostics remain.

The first fresh run with this corrected entry, `two-provider-global-r144`,
started the five-node MiniNDN topology and reached both Provider `READY`,
signed ACK offers, and `GRANT_VERIFICATION` at `BEFORE_ASSEMBLY`. It then
stopped at `RESOURCE_BOUNDARY:ownedSwap` with
`ownedSwapBytes=271720448` against `268435456`; cleanup passed. No requester
Selection, assembly, execution, terminal response, numerical oracle, repeat,
or qualification result exists. Evidence is in
[r144 static-routing owned-swap evidence](evidence/b189-r144-static-routing-owned-swap-20260920.md).
T003, T005, T006, T007, and T009 remain `PARTIAL`; the next run must use a
new run ID and a host state that stays below the unchanged resource limits.

执行顺序：`B189-0 → B189-1 → B189-2 → B189-3 → B189-5`。
保留历史 ID 稳定链接；序号不再代表时间。
成员、五 lane、动态检查、唯一结果记录见 [batch-execution.md](batch-execution.md)。
达到批次出口即验证，不为了少编译加入新职责。
