# Spec184 T007 A4 Native Offer Admission Coverage

**Date**: 2026-09-12  
**Batch**: `B5-A4-OFFER-ADMISSION-20260912`  
**Status**: `CLOSED_FOR_VALIDATION` for this focused C++ test unit; T007 remains `PARTIAL`

## Scope and boundary

本批只补充 `NativeOfferAdmission::verify` 的 C++ 负例和不可变投影覆盖，没有修改生产实现、
Core 消息契约、冻结的 Spec182 case manifest 或模型配置。测试覆盖认证字段缺失、wire digest、
key namespace、签名者/Provider/Service policy、request/model/graph binding、policy snapshot、
时间窗口、已过期 deadline，以及通过校验后 immutable planning view 的所有权边界。

本机仍只能运行 `Qwen3-0.6B` smoke/ABI 范围；本批没有加载 0.6B 权重，也没有声称
`Qwen/Qwen3.6-27B` qualification。该模型行继续为 `WAITING_EXTERNAL_INPUT`。

## Five coverage lanes

| Lane | Evidence in this batch | Boundary |
| --- | --- | --- |
| production entry/callers | CodeGraph/source review of `NativeOfferAdmission::verify` and callers in `NativeRequestPlanner`, `NativeGroupKeyAdmission`, `NativeInferenceClient`, and existing unit tests | verifies the named production selector and its call sites; no process qualification |
| implementation/wire | `NativeOfferAdmission.cpp`, `NativeObservedOfferV3.cpp`, canonical fixture JSON, and `NativeOfferAdmission.hpp` | checks the current Core-owned `ControllerVersion` and derived `offerDigest` contract; no invented DI field |
| test/harness/oracle | `tests/unit-tests/di-native-offer-admission.t.cpp`, frozen fixture vectors, selector `Spec182OfferAdmission` | 18 cases and 81 assertions passed in the same-tree C++ executable |
| compile/link/source closure | registered `unit-tests` target from `tests/wscript`; build includes the complete DI source closure and `NativeOfferAdmission.cpp` | compile/link passed with `/usr/bin/g++ -B/usr/bin`, Waf `-j4`; no full-tree rebuild claim |
| migration/evidence | T007 A4 row, qualification matrix and this record; Python is not the business oracle | no Python retirement, no no-Python process result, no real-model or external-owner result |

## Static review gate

逐小任务修改后，按项目静态门阅读完整 diff、production caller、fixture、测试注册和构建闭包，
并加载官方 read-only `review-agent`。使用的 skill/reference 摘要为：

```text
/home/tianxing/.codex/skills/review-agent/SKILL.md
sha256:07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228
skills/speckit-code-design/references/pre-test-static-review.md
sha256:6b5aff9458d1da93a6b0021ca5713aba58c14006654edef28744ed445259b486
skills/speckit-code-design/references/review-agent.md
sha256:1974ac9454ca727b264e22c391d5c98e1663622e31ca01825b12a67ad34cc16b
```

静态结果没有发现新增的 ownership、并发、异常边界、测试注册或链接缺陷。审查保留两个
Spec182 历史计划名与当前契约不一致的记录，而没有伪造生产字段或反向修改冻结 manifest：

* `SealCoreRejectsProviderReusedAcrossRoles` 与当前设计相反；同一 Provider 跨角色复用由
  `SealCoreAllowsProviderReuseAcrossRoles` 和 `NativePreSplitFirstPlacement` 覆盖。
* `RejectsMissingControllerVersionOrOfferDigest` 不能直接映射到 admission API；
  `ControllerVersion` 由 Core 消息契约持有，`offerDigest` 由已验证 payload/auth context
  派生。当前 admission selector 对应覆盖的是 request/model/graph binding、wire digest 和
  签名/策略拒绝。

## C++ result

The test source hash is:

```text
tests/unit-tests/di-native-offer-admission.t.cpp
sha256:8920e9fa087f8a1cd956d369397a6d85f878342fbe56b4679cf62815830b2c49
```

The affected target was built in `build-spec184-b6-candidate-tests`:

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin:$PATH /usr/bin/python3 ./waf \
  -o build-spec184-b6-candidate-tests build --targets=unit-tests -j4 -v
```

The build exited `0` in 24.539 seconds. The raw build log is
`.codex-tmp/spec184-a4-offer-admission-build-20260912.log` with SHA-256
`f743550810471fb15005274d839a570871b0e579bad63dadb394a33c5c5bceae`.
The resulting `build-spec184-b6-candidate-tests/unit-tests` binary has SHA-256
`f1a2417d6ec142bd5c3ed2ffe30c217d77e886b91a16874071d8e6980a38183a`.

The focused selector was run as:

```text
timeout 180s build-spec184-b6-candidate-tests/unit-tests \
  --run_test=Spec182OfferAdmission --report_level=detailed --log_level=message
```

It exited `0`: **18 test cases passed and 81 assertions passed**. The raw run log is
`.codex-tmp/spec184-a4-offer-admission-run-20260912.log` with SHA-256
`f3ccb2cd5abcb1ec02d0773c5bfaba882b685f031dfe5cf5f10c9d54600a0dd4`.

## Unsuppressed sanitizer result

The same source was rebuilt in the independent `build-spec184-i02-asan-r2` tree with
`-fsanitize=address,undefined`, `/usr/bin/g++ -B/usr/bin`, and Waf `-j4`. All 192 build tasks
completed successfully in 9m19.243s. The build log SHA-256 is
`47251a47c4f20aba1f408203949b29711988eb0bf5f07348acc85e2c9f18c6fa`, and the resulting
`unit-tests` binary SHA-256 is
`268697a1e96f5e861339cced7708f976c2d1a86257b90813460abd098e5490f3`.

With `ASAN_OPTIONS=detect_leaks=1:halt_on_error=1:strict_string_checks=1` and
`UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1`, the same
`--run_test=Spec182OfferAdmission` selector exited `0`: 18/18 cases and 81/81 assertions
passed, with no ASan, UBSan or LeakSanitizer report. The raw run log is
`.codex-tmp/spec184-a4-offer-admission-asan-run-20260912.log` with SHA-256
`f3ccb2cd5abcb1ec02d0773c5bfaba882b685f031dfe5cf5f10c9d54600a0dd4`.

## Batch retrospective and closure decision

| Gate | Result | Unobserved or remaining boundary |
| --- | --- | --- |
| `static` | `PASS` | historical manifest mapping gaps are explicitly recorded above |
| `compile-link` | `PASS` | normal and independent sanitizer trees linked the complete unit target |
| `runtime-test` | `PASS` | focused C++ admission selector only; no process/no-Python or model run |
| `asan-ubsan` | `PASS` | focused admission selector has no sanitizer or leak report; broader lifetime/process rows remain open |
| `unobserved` | `RECORDED` | parser fuzz breadth, full process qualification, Python retirement, Qwen3.6-27B and SIF/Tiger remain open |

The batch is closed for this test-coverage unit. It provides current C++ admission evidence for
the affected rows but does not promote T007, change A3, or make the local 0.6B capability a
27B substitute. Because the test harness and validation documents changed, the promotion contract
requires a fresh candidate identity before the next final qualification run; this record does not
silently rebind prior YOLO results to a new candidate.
