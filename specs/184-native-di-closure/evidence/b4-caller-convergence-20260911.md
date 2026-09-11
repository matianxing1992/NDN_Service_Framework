# Spec184 B4 Caller Convergence

**Status**: CLOSED_FOR_VALIDATION / focused caller and mode closure; formal qualification remains pending
**Batch**: B4 / T005
**Source baseline**: `865e1ee2`
**Candidate**: `865e1ee2`

本批把维护入口按真实 owner、有效配置和可观察出口收敛为 caller matrix 的 12 行。它只
关闭路由、接线和兼容边界的开发验证，不宣称 YOLO/Qwen 跨进程、no-Python 或最终资格完成。

## Batch growth decision

B4 在 B3 的 checkpoint export 稳定出口之后开始，成员限定为五组维护入口及其模式：YOLO
requester/provider、Qwen requester/provider 和 harness/collector。每行共享 caller/mode
路由判据、C++ selector 或明确的 compatibility 证据；没有新增 native 状态机，因此没有
为 launcher/collector 重复建立 sanitizer 树。C2 provider preparation 的旧 D2b selector
在运行时没有产生任何 role/response/output，首个边界见下文；该 selector 不再作为当前
caller matrix 的 oracle，改用同一生产 owner 的 post-selection/assembly selectors，并保留
失败原始日志供 T006/T007 收敛。

## Dynamic gate card

| Field | Value |
| --- | --- |
| Risk class | `routing/lifetime` |
| Dynamic profile | `none` for caller-only dispatch and process launchers; async owner profiles are inherited from B1/B2 |
| Parameter matrix | native config present; explicit compatibility selection; missing/invalid config; caller shutdown/child cleanup |
| C++ selectors | `ProductionIngressRunsNativeFourRoleTwoDeviceAssignmentRequestResponse`; `ProductionIngressRunsNativePostSelectionAssignmentFetch`; `NativeProviderIssuesCanonicalPreparationOfferV3`; `Spec182R10B73NativeConfigQwenRealProviderStream`; `Spec182R10B80NativeConfigQwenRealProviderConversation` |
| Business invariants | native route is explicit; compatibility is explicit; no planner/provider fallback; provider identity and role binding stay request-scoped; child/launcher cleanup is bounded |
| Repeat / budget | one focused run per selector; inherited B1/B2 sanitizer repeats remain authoritative for shared async owners |
| Toolchain / source identity | `/usr/bin/g++ -B/usr/bin`, system Boost 1.71, candidate source `865e1ee2`; normal integration binary digest `25df91d2cb2d1f4a486db637194b0b39bf999bea63cd81c78476d23b461394cf` |
| Output | raw logs under `.codex-tmp/spec184-b4-*`; no transient logs committed |

### Bounded Dynamic Parameter Matrix

| caseId | Parameter tuple / boundary | Expected business result | C++ assertion / selector | Status |
| --- | --- | --- | --- | --- |
| D-B4-01 | native config + canonical package/registry/tensor | native requester route reaches the C++ ingress and one response boundary | `ProductionIngressRunsNativeFourRoleTwoDeviceAssignmentRequestResponse` | `FOCUSED_BEHAVIOR_PASS` |
| D-B4-02 | post-selection assignment with provider/artifact identity | preparation is created after selection and the provider owner accepts the bound projection | `ProductionIngressRunsNativePostSelectionAssignmentFetch`; `NativeProviderIssuesCanonicalPreparationOfferV3` | `FOCUSED_BEHAVIOR_PASS` |
| D-B4-03 | Qwen stream native config + pinned tokenizer identity | native stream request returns the C++ request result marker | `Spec182R10B73NativeConfigQwenRealProviderStream` | `FOCUSED_BEHAVIOR_PASS` |
| D-B4-04 | Qwen conversation native config + continuation state | native conversation route returns the C++ request result marker | `Spec182R10B80NativeConfigQwenRealProviderConversation` | `FOCUSED_BEHAVIOR_PASS` |
| D-B4-05 | native config absent or unsupported mode | explicit compatibility/rejection branch; no silent native fallback | Python route/legacy exclusion tests plus source scan | `FOCUSED_BEHAVIOR_PASS` |

本批没有新增异步 owner，故动态 profile 对 caller 行为记录为 `none` with reason；这不是
“动态工具通过”。B1 的 TSan 和 B2 的 ASan/UBSan 证据继续覆盖被 caller 调用的共享 C++
状态机。B4 只验证矩阵中的路由和接线 case，模型真实输入、跨进程 no-Python 和资格负例
转交 B5。

## Coverage matrix

| Lane | Result | Evidence / query |
| --- | --- | --- |
| production entry / callers | covered | `contracts/caller-matrix.md` 12 rows；`rg -n "APPClient|request_task|request_streaming|request_native|APPProvider|execvpe|di-native-provider" examples/python/NDNSF-DistributedInference Experiments NDNSF-DistributedInference/ndnsf_distributed_inference` |
| implementation / wire | covered | CodeGraph exploration of `APPClient.request_native`, YOLO/Qwen native helpers, native provider launch branches; source paths and line ranges are in the matrix |
| test / harness / oracle | covered | C++ selectors listed above; `tests/python/test_ndnsf_di_app_sdk_compatibility.py`, `test_spec182_legacy_exclusion.py`, `test_spec181_native_production_launch.py`, `test_spec181_native_backend_registration.py` |
| build / source closure | covered | normal integration target and `di-native-provider`; provider binary digest `ff00c53e5df7aa1c9d428712f01542516162c71e7bb5a9db1931a43733b089b1`; native shared digest `1f99dbd6a691ff516d53f395b6ef50f72dd6264dbd7e46cc0c201b627c591677` |
| migration / evidence | partial | explicit compatibility and rollback rows are present; Python retirement, no-Python and real-model evidence remain B5 obligations |

## Validation

### Focused C++ selectors

With candidate-first `LD_LIBRARY_PATH`, the following selectors exited `0` and reported no Boost
test errors:

- `Spec170NativePostSelection/ProductionIngressRunsNativeFourRoleTwoDeviceAssignmentFetch`
- `Spec175NativeAssembly/NativeProviderIssuesCanonicalPreparationOfferV3`
- `Spec170NativePostSelection/ProductionIngressRunsNativeFourRoleTwoDeviceAssignmentRequestResponse`
- `Spec170NdnsfDiCoreFlow/Spec182R10B73NativeConfigQwenRealProviderStream`
- `Spec170NdnsfDiCoreFlow/Spec182R10B80NativeConfigQwenRealProviderConversation`

The corresponding raw output hashes are `570f95f6758d4fdae6373f793fa41b27294fd335d8f9f3ba92ee2d1086968753`,
`2106e52606f7e82e04974d92c2872015a196f6fdfe02f095c4c01502e7a53d55`,
`e302bb3e31a3829e81df065ea663a9ef4bbda7de3fd1260a83bc6fb19ceeb125`,
`f36c71bc9c6f24496e0de2c63a7280b778c4efc4c83131dc0cfbf5e98fbedf39` and
`ea764c5aec12d96641aa532fdb3cd7f712a1d35600ab6741522e4f7717af9fb8`.

### Python routing regression

After building the current candidate provider and setting `SPEC181_NATIVE_PROVIDER_BINARY`,
the four route/compatibility suites passed **38 tests in 2.99 seconds**. The first run without
that variable failed five tests before product execution because the default
`build-system-j2/examples/di-native-provider` path did not exist; that provenance boundary is
retained in `.codex-tmp/spec184-b4-python-tests.log`, and the corrected run is
`.codex-tmp/spec184-b4-python-tests-rerun.log` (SHA-256
`4d57fa15ed0767345df1833523441631222037b00a754e6b4f55c5074d2662fe`).

### Runtime/test miss retained

The older `Spec170NdnsfDiCoreFlow/ProductionNativeHandlersRunD2bRequestToFinalResponse`,
`ProductionNativeHandlersRunStreamedD2bRequestToFinalResponse`, and
`ProductionNativeHandlersPrepareRolesAfterSelection` selectors all reached
`NDNSF_INTEGRATION_BOOTSTRAP_READY` but then observed zero response publications and no role/output
records. The first failure is the test oracle at `ndnsf-di-core-flow.t.cpp:4751` (`0 != 1`),
followed by missing `/Backbone`, `/Head/Shard/0`, and `/Aux` observations. This is a real runtime
test miss, not a protocol PASS or a static-gate failure. Raw hashes are
`05951623fb8bb11ce6e6a2b7b004047114b1ab2fa80acc3a4ed0dccc1358e397`,
`22fc518c276830c8f4f2f6d758bc5658bf37f2d62e85373d161676d6f399885d` and
`f9ad67fe1c04503e1bf658d58a400ec146faa92a8aa249ffeef9daddbffbe2a4`.
The current matrix uses the passing maintained post-selection and assembly selectors instead;
the D2b behavior and real process/no-Python outputs remain open in T006/T007.

## Review trace and closure

The read-only review used `/home/tianxing/.codex/skills/review-agent/SKILL.md`, SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`, against baseline
`865e1ee2`. The reviewed diff is the caller matrix plus its source/selector evidence; no product
source was changed in B4. Findings were the stale provider selector and the missing default
provider path, both corrected by changing the matrix selector and overriding the candidate path;
the older D2b runtime miss stays linked above. `Closure decision: CLOSED_FOR_VALIDATION` because
all current matrix selectors and route regressions have stable focused exits. `QUALIFICATION_PASS`
is explicitly not claimed; B5 must close the open migration/no-Python/real-model rows.

## Batch retrospective

- `static`: explicit native/compatibility branches, no planner fallback, and launcher ownership were visible after the row refresh.
- `compile/link`: the only miss was a stale default provider path; candidate build and digest were then bound into the rerun.
- `runtime/test`: the inherited D2b selectors still fail at zero response/role observation; this boundary is preserved rather than relabeled.
- `unobserved`: real YOLO/Qwen model execution, no-Python qualification, and external Tiger/SIF rows were not run.

