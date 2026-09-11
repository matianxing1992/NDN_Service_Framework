# Spec184 T007 C++ Process Qualification Refresh

**Date**: 2026-09-11
**Status**: `PARTIAL` / current-candidate process evidence; final qualification remains open
**Build**: `build-spec184-b5-candidate`; target rebuild `r2` completed with `/usr/bin/g++ -B/usr/bin`, `-j4`

本记录把当前候选的 C++ 主导跨进程结果集中绑定到 T007。Python 只负责生成私有
PIB/TPM、启动和回收进程；请求、授权、Provider 执行、ORT 推理、token/数值 oracle
均由 C++ 可执行文件完成。所有 run root 均保留在 `.codex-tmp/` 或 `/tmp/`，没有把
原始秘密或瞬态产物加入 Git。

## Candidate target and runtime closure

The final target invocation was:

```text
WAFLOCK=.lock-spec184-b5 ./waf -o build-spec184-b5-candidate build \
  --targets=unit-tests,integration-tests,DI_NativeRequester,di-native-provider,
  App_ServiceController,DI_NativeArtifactAuthority,spec182-native-grant-requester-process -j4
```

The corrected target command completed successfully; the earlier request for the non-generator
name `DI_NativeOnnxAssemblyWorker` is retained as a Waf task-resolution boundary in the failure
log. The final C++ unit and integration sweeps both exited `0` and ended with
`*** No errors detected`:

| Check | Result | Evidence |
| --- | --- | --- |
| candidate target build | `PASS` | `.codex-tmp/spec184-final-target-build-20260911-r2.log`, SHA-256 `c68038b4fd6973eb55a670907c4bc896bb8f8386cab1064602be714b7cd12fe3` |
| full `unit-tests` | `PASS`, exit `0`, 78.568 s | `.codex-tmp/spec184-final-unit-20260911-r1.log`, SHA-256 `b4691658f3053a51f6b11ca9299e581760ff50b5ede761f5773ad16b5002a6cd` |
| full `integration-tests` | `PASS`, exit `0`, 232.724 s | `.codex-tmp/spec184-final-integration-20260911-r1.log`, SHA-256 `dee9e3b7f5235f5d2c337bf0b98326f9971935116db2616e74c94662d7d6d076` |
| Spec182 C++ harness regression | `PASS`, 71 tests | `.codex-tmp/spec184-python-harness-20260911-r1.log` (`test_spec182_native_closure`, `test_spec182_legacy_exclusion`, `test_spec182_native_bindings`) |

The six business binaries have no `libpython`/`python3` `DT_NEEDED` entry, no matching Python
runtime string, and `ldd -r` reports no unresolved symbol:

| Binary | SHA-256 |
| --- | --- |
| `examples/App_ServiceController` | `50667da49f9f190304fc2de09114b23bc46e0afba4fba143035bd2e5db0940d1` |
| `examples/DI_NativeArtifactAuthority` | `b0840be47108ff78c1241fe480689e167dc175e8bf9ce146b2622be01b3d792e` |
| `examples/DI_NativeRequester` | `7ca9f17803649539df78eaa8d03fa66a84ca1fa6e5098b8d21a1b426b5ef4c50` |
| `examples/di-native-provider` | `0a13e7ce70d30be51242b04d252eb0aae11cdbb8abee259bcb2997df71746b18` |
| `DI_NativeOnnxAssemblyWorker` | `e44bd0a31205d3106461d6d48884da70adb4e8acc8ef5fea8753573964aff283` |
| `spec182-native-grant-requester-process` | `02c0e24da191fbb4fa70120862966c895aaa30339bddecd94e752b690b0af853` |

## C++ business process results

Each result below is a fresh run against the current candidate. The process scripts are harnesses;
the named markers are emitted and asserted by the C++ requester, Authority, Provider, or worker.

| Case | Result | Raw C++ evidence |
| --- | --- | --- |
| YOLO protected unary | `PASS`; `NATIVE_NUMERICAL_ORACLE_PASS` values `4,0,12` and `NATIVE_REQUEST_SUCCEEDED` | `.codex-tmp/spec184-process-unary-20260911-r2/requester.log` SHA-256 `bfc3b3276f86522b4be7743932f0aa56b3d70fa2bb1abb19e2e4cbb127665ef2`; Provider log `04ecaa1cdfa3269d2f617b88fde96a428f65b6b706ab20da2fc7c3085390c1a9` |
| Qwen tiny stream | `PASS`; 8 token events, terminal oracle and request success | `.codex-tmp/spec184-process-stream-20260911-r1/requester.log` SHA-256 `b4234811675655641bab0923f26d19795b340d954a1285a0760f50106510c4ba`; Provider log `e478a8c21c8573d844dce775f4612515aa3677fa0f5e7e9da687b5ab530686cb` |
| two-turn continuation | `PASS`; checkpoint written, second turn succeeded, wrong parent rejected | requester logs `3357d1303855a3e486dbb2712c4e17b7d249561d128f51a865d040525199994d`, `779fda2109f2dd2a2d78c6da85911a2b747aef44af1a3f0998e0f7ba8ecb2b00`, `cafe6b9e31a2d4676d9ab384514cebbc2d6e5818439eaba98c49833b2bf1871c` |
| Provider recovery | `PASS_FOR_ROW`; first turn succeeded, Provider restart caused `PROVIDER_CONVERSATION_STATE_MISSING`, wrong parent rejected | `.codex-tmp/spec184-process-recovery-20260911-r1/`; requester hashes `c183a8d1d8ce000ad20509fbcbf70a39017810392f59714d8c2af6c3e245b52c`, `3a47df8022cfeba178820cc1f9abe1508d88ee5d68609f417f448b2c93c55c47`, `f1e8024d92c67a8f96727346132cfdf6d17fefa8fa8135c09aad213961950be1`, restart hash `e1ed7d48732d1ed22e0498e7bb99f20dc990519fa2e5c3d43585958d5311a9d8` |
| Provider replacement | `PASS`; first Provider stopped before execution and Provider-B completed attempt 2 | requester `fc3c904684b811775d04f5073c9e730b4517362f8cc346023fe69b1b3f1501ed`; Provider-B `2e177dd0ef2ef61b41b3e8767863afae394dc644759558b023c62b41916f5aa4` |
| replacement without backup | `PASS_FOR_ROW`; terminal `DI_NATIVE_NO_ADMITTED_PROVIDER` | `.codex-tmp/spec184-process-replacement-nobackup-20260911-r1/requester.log`, SHA-256 `461893379d9d696f308b81c9e81f50a3be05f67fe22f2e06493e9519a84108f6` |

The replacement run's first Provider log is `d8cea73452cdca5ce860e424881445d62eda39c14e2123738a0f360d9ccc9a98`;
it contains no execution evidence, while Provider-B contains the authenticated attempt-2 evidence.

## Authority grant process matrix

The same candidate's independent C++ Authority/requester process launcher completed one positive,
five Authority-handler rejection cases, and one transport-unreachable case. Every launcher reported
`SPEC182_NATIVE_GRANT_PROCESS_ISOLATION=PASS`; the expected negative cases returned their declared
native rejection marker rather than a Python oracle result.

| Case | Native boundary | Probe log SHA-256 |
| --- | --- | --- |
| `positive` | signed grant accepted | `6d8e95dd795b7dc6f33ddc74c6418dc1228f4c7a2dd9dc04be1c4c46b9084962` |
| `bad-signature` | Authority rejects signature/policy | `531e7065f082f437deec6b25b502b079a14696af9435d58677f1a053be728920` |
| `wrong-epoch` | Authority rejects epoch/policy | `b053b7166bb4991cf06793f02ead7baccdf770f0509760286499c96892e9064d` |
| `unknown-recipient` | Authority rejects unconfigured recipient | `cef7bedd36a95415647af2a729ce9d648e75f554a0cd35a928b3f97f303845b7` |
| `malformed` | Authority rejects non-canonical request | `1637ba022c60598b6cf6fdb87321bb1f0acc79dafe0d1085c85f32413e8a964c8` |
| `expired` | Authority rejects expiry beyond policy | `3fe8a378ae09a3029fc8b093691869b65a53d45d71a91763329dbf788b48b3c3` |
| `unreachable` | requester reports transport timeout | `f5123c684d83dce257096e807712ea773ac8e52776a909222a9bc5d6676a9a8f` |

The retained grant run roots are `/tmp/spec182-r11-b1-process-8d0naebx`, `e8sipn0i`, `tq52nfgq`,
`24l14sex`, `do_j5695`, `9g6mi4ji`, and `mn4xqjn5`, in the table order above.

## MiniNDN owner and isolation evidence

The canonical two-node MiniNDN owner was rerun with the refreshed runner manifest
`.codex-tmp/spec184-b5-current-runner-manifest-r3-20260911.json` (SHA-256
`a1e7df48caf5837d690ad1847e3490704c309af739e0df318ef3062ac8fdfd13`). The current root run
`.codex-tmp/spec184-owner-po001-20260911-r9/` returned `PASS`, exit `0`; its result digest is
`e65fc1b507fe40cc275601b031c724b99f64a254ecaaa51272a7845f8e5509fd`, runner-result digest is
`d3f757c37e196de6e616554e65eaea324b3e8706efe142c2d9cd8f673ccedcd0`, and node-context digest is
`97d548d3a12e91718400faf604629f11fb2bbf44e0d1338d73ddc1d1be4e625f`.

The runner observation was complete with no policy or integrity violations and all required
evidence categories (`identity`, `process-tree`, `namespace`, `exec-map`, `endpoints`,
`business-oracle`, `cleanup`). No `PYTHON_EXEC`, `PYTHON_MAPPING` or undeclared endpoint was
observed. The non-root owner boundary and the earlier stale-manifest/digest attempts remain
historical `UNQUALIFIED` records; they are not reclassified as protocol failures.

## Qualification disposition

These results close the current candidate's bounded C++ process classes for unary, stream,
continuation, recovery, replacement, grant authorization and the `PO-001-stream` owner row.
They do not by themselves close every inherited I02–I08 detector/collector row: the remaining
rows require their declared counterexample or observation-completeness evidence, candidate-bound
matrix entries, and (where applicable) real-model or external SIF/Tiger ownership. T007 therefore
remains `PARTIAL`; T008 remains blocked by the qualification dependency, not by the process runs
recorded here.
