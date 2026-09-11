# Spec184 T007 Current Native Qualification

**Date**: 2026-09-11  
**Status**: `PARTIAL` / candidate-bound local qualification started; no promotion  
**Candidate**: `sha256:f7ee8f65a67a375f993e4db3a7558f22e996b8b170415b7d1325be89e3f32441`

本记录绑定 [promotion candidate](../contracts/promotion-candidate.md) 的当前源码、运行时、
fixture、harness 和配置身份。它汇总本地 C++ 资格边界，不能把局部 selector 或 Python
wrapper 回归提升为完整 Spec184 qualification。

## Candidate-bound results

| Lane | Command / selector | Result | Evidence |
| --- | --- | --- | --- |
| Full native unit | `build-spec184-b5-candidate/unit-tests --log_level=test_suite` with candidate-first `LD_LIBRARY_PATH` | `PASS`, exit `0`, 146.304 s, `*** No errors detected` | `.codex-tmp/spec184-b5-full-unit-20260911.log`, SHA-256 `143ecc81f846b9ef888e37563560aecd2d67bfae88f5378c9e34ae6902d167e3` |
| Full native integration | `build-spec184-b5-candidate/integration-tests --log_level=test_suite` with the same library path | `FAIL`, exit `1`, 48 Boost failures | `.codex-tmp/spec184-b5-full-integration-20260911.log`, SHA-256 `5244434ca84d77f2b6ae6909d237d96cf1f3a34b9282b14a090e85e173701793` |
| Component dynamic gate | Provider-host 8 cases under unsuppressed ASan/UBSan and independent clang TSan | `PASS`; no sanitizer or LeakSanitizer report | B5 component evidence and `.codex-tmp/spec184-b5-provider-fix2-asan-20260911.log` |
| C++ authority/native routes | `Spec184AuthorityIoOwnership`, `Spec184DurableOutcome`, native post-selection/assembly and Qwen stream/conversation selectors | `PASS` for the named focused selectors | B5 component evidence and its candidate binary hashes |
| Python orchestration regression | 71 harness tests | `PASS`, observation/runner only | `.codex-tmp/spec184-b5-python-harness-20260911/pytest.log` |
| MiniNDN owner/runner `PO-001-stream` | root owner + canonical two-node topology + current runner manifest | `PASS`, exit `0`; business marker and identity/namespace/process-tree/endpoints/cleanup evidence complete | `.codex-tmp/spec184-b5-owner-probe-20260911-r2/result/{result.json,runner-result.json,node-context.json,closure-run/trace.txt}` |

The full integration failures first reach the legacy D2b/D2h121/D2h212 response/role oracle with
zero observations and the Spec175 tiny-ONNX collector's `stream event gap exceeded retry budget`.
Spec184 authority selectors still pass in the same executable. These are retained as current
runtime/test boundaries; they are not reclassified as protocol failures or ignored qualification
rows.

## Process/no-Python boundary

The bounded process driver was invoked for `I01`:

```text
python3 tests/standalone/run-spec182-native-closure.py \
  --manifest tests/fixtures/spec182/case-manifest.json \
  --case I01 \
  --output .codex-tmp/spec184-b5-process-preflight-20260911
```

It stopped in preflight with exit `2` and result
`{"case":"I01","error":"manifest schema mismatch","status":"UNQUALIFIED"}`.
The supplied frozen manifest declares `spec182-case-manifest-v1` and has no `cases` list, while
the driver requires `spec182-native-case-manifest-v1`. No requester/provider process, namespace,
network request, business oracle, or cleanup result was observed. Raw boundary files are retained:

- `.codex-tmp/spec184-b5-process-preflight-20260911/result.json` — SHA-256
  `edb5b5566c73c900db9a8175b58bfebc037c3fbce4f89cb8741b79f9fa0c2d2f`;
- `.codex-tmp/spec184-b5-process-preflight-20260911/preflight.exit` — `2`;
- `.codex-tmp/spec184-b5-process-preflight-20260911/preflight.log` — empty because the driver
  records the first boundary in `result.json`.

This is a harness/schema `UNQUALIFIED` boundary. The corrected owner run used a current candidate
runner manifest and closed the bounded `PO-001-stream` process case; its first owner attempt used
the wrong runner case ID and stopped with `case id is not unique` before staging. Both raw attempts
remain under `.codex-tmp/spec184-b5-owner-probe-20260911*`. Repairing the manifest or driver is a
new candidate-affecting change and requires a fresh identity and convergence audit.

## Qualification decision

T007 remains `PARTIAL`: full native unit, component dynamic gates and one bounded MiniNDN owner case
pass, but full integration, I02–I08 process/no-Python rows, parser-fuzz, negative collector rows,
real-model/MiniNDN breadth, Python retirement and external SIF/Tiger execution are not qualified.
T008 cannot start, and no promotion or final handoff is authorized by this record.
