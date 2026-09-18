# B189-0/B189-5 Convergence Evidence

**Status**: IN_PROGRESS / BLOCKED_FOR_NATIVE_EXECUTION
**Updated**: 2026-09-18 02:22 -0500

## T001 integration boundary — 2026-09-18 13:29 -0500

本节为当前 T001 边界，取代下方历史初建状态；产品仍 PARTIAL。
基线 `2ee71559` 加现有未提交实现。CodeGraph explore 误匹配了 `.codex-tmp`
历史快照，因此本轮使用精确生产路径核对，未把快照内容当当前源码。

| Lane | State | Actual binding / remaining owner |
| --- | --- | --- |
| production/callers | covered | DI_NativeRequester.cpp:237-260 → User::prepare (Runtime.cpp:1332-1355) → configured RepositoryArtifactPublisher or canonical fallback；T003 接入实际 Repo owner；NativeCanonicalOnnxAssembler.cpp:305/353 consumer 由 T006 改为选定材料 |
| implementation/wire | covered | NativeProviderHandler.cpp:2842-2875 建立 NativeEpochCoordinatorConfig，保留 V3 roleSpecFromSelectionProjectionV3 endpoint，另追加 TOKEN_FEEDBACK；NativeEpochCoordinator.cpp:992 使用 executePreparedRoleAsync 并等待结果。T005 负责 ingress，T006/T007 负责有界材料/真实 handoff，不另造协议 |
| test/harness/oracle | covered | tests/unit-tests/spec189-repo-publication.t.cpp，tests/integration-tests/spec189-request-reference.t.cpp 与 spec189-placement-oracle.t.cpp；di-runtime.t.cpp / di-prepared-provider.t.cpp 提供 lifecycle 既有入口；examples/Spec189TwoProviderOracle.cpp 日志判据待 T007 修正。T008 host guard 属 Python 进程控制，native owner 断言仍用 C++ |
| build/source closure | covered | examples/wscript 注册 DI_NativeRequester/DI_NativeArtifactAuthority/spec189-two-provider-oracle/di-native-provider；复用 global-r3 tree。2026-09-18 重查三个 binary 摘要仍匹配下方 r2 表；readelf RUNPATH=/usr/local/lib:$ORIGIN/..，ldd 的 NDN/DI 与 ORT 分别为 /usr/local 和 /opt/onnxruntime，无 missing library。nm 确认 installed libndnsf-distributed-inference.so 导出 User::prepare 与 PreparedModel::request；记录 .codex-tmp/spec189-t001-resume/di-symbols.log |
| migration/evidence | covered | 原子层格式和旧 manifest 兼容由 T003 版本化；cold/warm 与 full-path 证据不混用。r25 FAIL/current boundary 见 architecture audit；候选不再假定与当前脏树一致 |

候选源记录复用 `.codex-tmp/spec189-qwen-two-provider-20260918/candidate/stage-manifest.json`：
revision `e6de91484c29aa9480d55605af694f39b081c455`、28 layers、float16，旧 stage
范围 [0,14)/[14,28) 仅作转换输入/对照；`expectedTopToken=null`，必须由 T007
补独立正确性 reference，不计已验。qwen_state_successor_pairs 校验动态 KV 名称/覆盖；
现有 policy/key/module/digest preflight 复用，T003 改材料格式后更新实际配置来源。

T008 固定安全策略入口为运行 profile（必须显式传给 native MiniNDN launcher），
采样周期 1 秒，memory/disk 最低余量与 swap 增量阈值必须有限且正数；guard 覆盖
准备/发布/请求到 cleanup。以受控阈值小 fixture 验证，实际峰值留 T009。
本轮 df 仅 1.4 GB 可用，MemAvailable 约 6.4 GB，禁止启动 full-model 或竞争构建；
后续安全门通过前先处理可重建产物容量。对受影响目标增量 -j4，不重编未变 Core/Repo。

**Four miss classes**: static=识别 CodeGraph 快照污染并用精确源码纠正；
compile-link=未构建；symbol inspection 最初误写 libndnsf-di.so，按 ldd 实际 SONAME
改查 libndnsf-distributed-inference.so，属于工具路径误写而非链接失败；
runtime-test=本轮无原生/模型运行；unobserved=T003/T005/T006/T007/T008/T009 的实际验收。

**Batch growth decision**: T001 只关闭实施映射；不等待 real-Qwen receipt/执行后才关闭，
这些已有明确后继 owner。**Closure decision**: CLOSED_FOR_VALIDATION for the documentation map;
next trigger is T008 guard implementation, not a model run.

**Review trace**: 官方只读 review-agent 已核对冻结 patch
`.codex-tmp/spec189-t001-resume/review/diff.patch`，SHA-256
`8bf86f92115cda9c2be17f4f612f1e0679602f725534244b2523cff1f2dcffc7`，确认 T001
出口满足，可转 T008。数量歧义澄清：本轮重查 requester、provider、oracle 三个 binary，
与下方四项历史表中的对应项匹配；authority 本轮未重新 hash，不冒称四项全重查。
11/11 技能同步、Spec pointer 前置检查与 active Context health 通过；无新 build/runtime。

Spec189 documents and the active pointer have been created. The structural
checker passed and `verify-spec-kit-sync.py --require-entrypoints` passed
(`11/11` local entrypoints plus personal shared skill). The candidate tuple,
real Qwen artifacts, affected native build receipt and r01-r21 MiniNDN logs are
retained. The complete production caller/symbol map, C++ full-path oracle,
post-grant execution boundary and repeat convergence are not complete.

## Four miss classes

- static: the initial plan lacked mandatory policy/credential/module/disk/digest preflight and a stable post-grant marker exit; the V3 endpoint projection issue was fixed after r18, but its C++ regression is still missing;
- compile/link: the affected DI target closure was rebuilt with the recorded receipt, but no Spec189 full-path oracle target has run;
- runtime/test: real ACK/Selection and protected-grant verification are observed in r21; no fetch, assembly, runner, execution, terminal or cleanup result is observed;
- unobserved: the first post-grant boundary, native Repo commit ownership, placement-bound fetch/assembly, hidden-state handoff, terminal output, drain and repeat.

## Five-lane coverage

| Lane | State | Evidence / gap |
| --- | --- | --- |
| production entry/callers | `covered-partial` | real requester/provider path and Selection callers are observed; prepare/Repo and post-grant execution closure are incomplete |
| implementation/wire | `covered-partial` | candidate policy, credentials and V3 projection fixes are present; C++ oracle and resource guard are absent |
| test/harness/oracle | `gap` | no registered Spec189 C++ full-path selector or repeat checker has run |
| build/source closure | `covered-partial` | affected DI binaries have a receipt; Spec189 oracle source/link map is missing |
| migration/evidence | `covered-partial` | immutable candidate and r01-r21 records are retained; repeat and cleanup evidence are missing |

## Closure decision

`OPEN_FOR_NEXT_BATCH`: implement and review the candidate preflight, provider
post-grant marker sequence and C++ V3 endpoint-preservation regression; then
run a fresh candidate to classify the first post-grant boundary before
attempting full execution. No task is complete from the current evidence.

## T001 native example/source closure r2 — 2026-09-18 11:20 -0500

The registered Spec189 example targets were rebuilt from the global dependency
configuration in `build-spec189-b189-3-global-r3` with `-j4`:

| Target | Source registration | SHA-256 | Loader boundary |
| --- | --- | --- | --- |
| `DI_NativeArtifactAuthority` | `examples/DI_NativeArtifactAuthority.cpp` / `examples/wscript:274-279` | `230b17f47cfb6cc55d346d704af393837315876954678258a44a2ce43f2756df` | `RUNPATH=/usr/local/lib:$ORIGIN/..`; DI/Core/NDN-CXX resolved through the global install |
| `DI_NativeRequester` | `examples/DI_NativeRequester.cpp` / `examples/wscript:253-258` | `8422a72d22344ca4f8f5f66896546864e93771bc68e82d190c88146bd43a7a19` | same global host RUNPATH; `PreparedModel::request` is an unresolved production-library symbol |
| `di-native-provider` | registered provider example / `examples/wscript:531-536` | `0dfab1316ceda264ff889be21647e75b53ae09db259493d825862975c6d680b0` | global DI/Core/NDN-CXX, Boost 1.71 and ONNX Runtime closure; no checkout or temporary prefix |
| `spec189-two-provider-oracle` | `examples/Spec189TwoProviderOracle.cpp` / `examples/wscript:285-290` | `2607cdfb2050edbf5109e73c46ad53f304f78ea39ccc62d103eca1818ee2dec1` | standalone C++ oracle with the same global RUNPATH policy |

The production symbol map is present in
`.codex-tmp/spec189-t001-example-closure/symbol-and-identity.log` and points
to `Runtime.cpp` (`User::prepare`), `PreparedModel.cpp`
(`PreparedModel::request`), `NativeRequestEnvelope.cpp`
(`encodeNativeRequestEnvelope`), `NativeCanonicalArtifactPublisher.cpp`
(`prepare`, `publishUncached`, `bindPrepared`), and
`NativeProviderHandler.cpp` (provider execution/runner preparation). The
example closure build passed in
`.codex-tmp/spec189-t001-example-closure-build.log`. The exact hash-to-loader
association is recorded separately in
`.codex-tmp/spec189-t001-example-closure/loader-identity.log` (SHA-256
`9a536b77dc542e0a22ac4c2912d9979940d48fe788ba5e660e647260e47edc84`): each
of the four listed hashes was checked with `readelf` and `ldd`; NDN/NDNSF
libraries resolve from `/usr/local`, Boost 1.71 from `/lib/x86_64-linux-gnu`,
and ONNX Runtime from `/opt/onnxruntime`, with no `not found`, checkout or
temporary path.

This closes the registered source/build portion of T001 and confirms that the
native examples use the documented global dependency roots. It does **not**
close the candidate handoff identity, a real Qwen receipt through
`Runtime::prepare`, or the post-grant execution path; T001 remains `PARTIAL`
until those production-path facts are recorded.

The frozen documentation snapshot
`.codex-tmp/spec189-t001-convergence-review-r2/diff.patch` (SHA-256
`2a2b7019da75383e1290f5c4f21f7f9f372a6b4d0424fd310b5eb425d02f6109`) received
the official read-only review-agent result `STATIC_PASS`, with no P0/P1/P2
finding. The review did not build or run the selector.
