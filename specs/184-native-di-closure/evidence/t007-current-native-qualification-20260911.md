# Spec184 T007 Current Native Qualification

**Date**: 2026-09-11  
**Status**: `PARTIAL` / candidate-bound local qualification started; no promotion  
**Candidate**: current identity is recorded in [promotion candidate](../contracts/promotion-candidate.md); the earlier `sha256:311d23ecf6b7c8fa8f1f69a309a5855b3f969844279a2250d4dcf9c1557b8a98` record is historical and superseded by the T007 process refresh

本记录绑定 [promotion candidate](../contracts/promotion-candidate.md) 的当前源码、运行时、
fixture、harness 和配置身份。它汇总本地 C++ 资格边界，不能把局部 selector 或 Python
wrapper 回归提升为完整 Spec184 qualification。

## Candidate-bound results

## Focused C++ runtime boundary after the full sweep

The current candidate was rerun with the parent-qualified C++ selector
`Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnxI01OneProvider`, native runtime
timing, and `NDN_LOG='*=TRACE'`. Both attempts stopped at the same first
production boundary (exit `201`) before a request reached the ONNX runner:

```text
NDNSF_DI_EPOCH_COORDINATOR phase=epoch_start role=/LLM/Pipeline/Stage/0 epoch=0
NDNSF_DI_NATIVE_FAILURE ... reason=native epoch coordinator is missing its canonical token input
Collaboration role /LLM/Pipeline/Stage/0 failed: native epoch coordinator is missing its canonical token input
```

The collector subsequently reports `stream event gap exceeded retry budget`.
This is retained as a C++ production handler/configuration boundary, not as a
stream protocol result. The raw runs are preserved at
`.codex-tmp/spec184-tiny-i01-diagnose-20260911/` and
`.codex-tmp/spec184-tiny-i01-diagnose-20260911-r2/`; the second `run.log` has
SHA-256 `d07c4009a28e6c7df4ce0e0b4b2c105f62dbf0b29a9390aefadcc911267be9c1`.
The first diagnostic trace was inconclusive about whether the projection
mapping was lost. A default-off handler trace in rerun `r3` records
`input_count=2 application_input=true edge=APPLICATION_INPUT@request-input
edge=TOKEN_FEEDBACK@group-spec175-i01`, so the signed projection reaches the
handler correctly. The remaining boundary is now the request-input payload or
its handoff into `NativeEpochCoordinator`; the next repair must identify why
the encoded `input_ids` bundle is absent at `tokenIdsFromInputs` before another
qualification retry. The `r3` raw log SHA-256 is
`637884258ff60887e4a64b26c98f03a887fab4f81ac7b09bddc9906614cb33e2`.
The follow-up `r4` run added payload diagnostics but still emitted no
`NDNSF_DI_INPUT_BUNDLE` record before the coordinator failure. Its raw log
SHA-256 is
`49a8323c50da10f4c6655b4d45c7f3233f6cb294f2e3d926c2f140811253d9f7`.
The production fixes were then rerun as `r6`: the same I01 selector exited
`0`, reached epochs 0 through 7, emitted eight token events, and produced a
terminal `NDNSF-DI-FINAL-V1` EOS payload. The raw log SHA-256 is
`4c4015b900e2ccf1f5f7706fe3622a1d7e1aa68a11be82ae84c23c778eae850e`.
This closes the I01 first-boundary defect only; the remaining I02+ matrix
selectors still require the same bounded C++ dynamic loop.
After repairing the alias collision, rerun `r5` confirmed the bundle is
present and decodes as `tensors=input_ids`, then reached the next C++ boundary:
`NativeProviderRuntime requires a runner preparation callback`. This is the
preassembled compatibility path entering the epoch coordinator without a
preparation factory; its raw log SHA-256 is
`61a75cf871432eb57158a9dc07de05cd3e787aa96873bdb5b7ee46b44dd97744`.

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

## Spec175 C++ dynamic batch after the production fix

The parent-qualified selector batch
`Spec170NdnsfDiCoreFlow/Spec175NativeTinyOnnx*` was rerun against the rebuilt
candidate with `-j4` artifacts and candidate-first libraries. The batch exited
`0` with `*** No errors detected`. Positive I01/I02/I03/I04/I05/I06/I11/I12/
I15/I16 cases completed their native stream or replacement oracle; the expected
negative I07/I09/I10/I13 cases reached their declared failure boundaries and
were accepted by the C++ test assertions. Raw output is retained at
`.codex-tmp/spec184-tiny-batch-20260911/run.log`, SHA-256
`29773f5d89ce9192bf18be80080bf3a08acf6dd7165bcf7ab78f51326cea43ef`.
This is a bounded C++ dynamic `PASS` for the sampled Spec175 behavior classes,
not final Spec184 qualification; process/no-Python, inherited negative rows and
external owner closure remain open. The observed-offer parser sample is recorded
separately below.

## Full native sweep after the production fix

The rebuilt candidate then ran the complete C++ integration and unit
executables with candidate-first libraries. Both exited `0` with
`*** No errors detected`:

| Executable | Result | Raw log / SHA-256 |
| --- | --- | --- |
| `integration-tests --log_level=test_suite` | `PASS`, exit `0` | `.codex-tmp/spec184-full-integration-20260911-r2/run.log` / `8b74652a9a9e13aec564e1bb106dca1bf2a724c7a4d84ca00a1209709ad910c8` |
| `unit-tests --log_level=test_suite` | `PASS`, exit `0` | `.codex-tmp/spec184-full-unit-20260911-r2/run.log` / `16037133632b55d5894b88672fd2d32598955822aab9f82b2e90d1751e3910ed` |

This closes the complete local C++ unit/integration sweep for the pre-refresh
source tree. The fresh candidate rerun is recorded below; neither sweep closes
process/no-Python, inherited negative rows or external SIF/Tiger ownership.

## Fresh candidate rebuild and full native sweep

After removing only reproducible untracked build directories, the candidate was
configured with system-first `/usr/bin/g++ -B/usr/bin`, `.lock-spec184-b5`, and
`-j4`. The explicit Spec184 target closure completed `502/502` tasks; the
assembly worker and five helper binaries completed `15/15`. A broad Waf build
was not used as a Spec184 result because it first reached the unrelated historical
`spec181-assembly-parity` link target; that boundary is retained in
`docs/failure-log.md`.

With candidate-first `LD_LIBRARY_PATH` and
`NDNSF_SPEC182_BIN_DIR=build-spec184-b5-candidate`, the fresh executables both
completed without Boost failures:

| Executable | Result | Raw log / SHA-256 |
| --- | --- | --- |
| `unit-tests --log_level=test_suite` | `PASS`, exit `0`, `*** No errors detected` | `.codex-tmp/spec184-candidate-unit-20260911-r3/run.log` / `127d751be7522d0f7b3c8b3968bae393ae7d08082debf0b6d6bb65dfe6fcbb4d` |
| `integration-tests --log_level=test_suite` | `PASS`, exit `0`, `*** No errors detected` | `.codex-tmp/spec184-candidate-integration-20260911-r1/run.log` / `f0fccf442f6de69ab6a5e585e1eb84eb470aa1fad49e2316d6ca0210c2962027` |

The fresh runner manifest is
`.codex-tmp/spec184-b5-current-runner-manifest-r2-20260911.json` (SHA-256
`4212ca6f81913f30c10c23b1a5d3fcfa6d6a172de295b6e5a67fd3b2c23a7826`). Its
first current-user owner probe stopped before topology creation with exit `2` and
`MININDN_REQUIRES_ROOT` (`uid 1000`), which is retained as a privilege boundary.
With the authorized root owner and `/usr/local/bin` restored in `PATH`, the same
manifest completed the canonical two-node `PO-001-stream` case:
`.codex-tmp/spec184-b5-owner-probe-20260911-r7/result.json` (SHA-256
`e65fc1b507fe40cc275601b031c724b99f64a254ecaaa51272a7845f8e5509fd`), runner
result SHA-256 `7cc45f6f9d4487ffe45de90dff37c6bc2e28e030295d69e6b31aa2c48186a0b9`,
exit `0`, complete node/namespace/process/endpoint/cleanup evidence and business
marker. The failed root retry that omitted `infoconv` from `PATH` is retained as
`.codex-tmp/spec184-b5-owner-probe-20260911-r5-owner.log`; the corrected command
explicitly includes `/usr/local/bin`.

The current-user result remains preserved at
`.codex-tmp/spec184-b5-owner-probe-20260911-r4/result.json` (SHA-256
`004cfa0a88d00667c2c520c163a5c1461ec40a81fe3d39cacb703075aaf93fc0`). The
historical root owner PASS is tied to the previous candidate and is not reused;
the r7 result is the fresh candidate-bound owner evidence.

## Sanitizer boundary for the shared tiny-ONNX batch

### I02 ownership-cycle repair

Static review of `ServiceProvider::fetchCollaborationSignedExactData` found a
production callback ownership cycle: `express` strongly captured `retry`, while
`retry` strongly captured `express`. The retry closure now keeps a `weak_ptr` to
the express closure and promotes it only while scheduling another attempt. This
preserves cancellation/deadline behavior and allows the callback graph to be
released after the terminal result.

The affected candidate was rebuilt with the system-first `/usr/bin/g++ -B/usr/bin`
toolchain and `.lock-spec184-i02-asan-r2`; the same named I02 selector was run in
an independent unsuppressed `asan-ubsan` tree. C++ assertions passed, process exit
was `0`, and the log contains no ASan, UBSan or LeakSanitizer report:

| Check | Result | Evidence |
| --- | --- | --- |
| `ServiceProvider.cpp` static ownership review | `STATIC_PASS` | `codegraph node ServiceProvider::fetchCollaborationSignedExactData`; complete diff reviewed; strong cycle removed at lines 7031–7059 |
| I02 sanitizer selector | `DYNAMIC_PASS`, exit `0` | `.codex-tmp/spec184-i02-asan-20260911-r2/run.log`, SHA-256 `eb8b2cb46afedaf701b53a52a3e7fd7264fc1ceccf1b0cfc7f355b461403b0d4` |
| I02 candidate rebuild | `PASS`, `120/120`, 5m50.113s | `build-spec184-i02-asan-r2`; command recorded in this evidence and Waf lock hash bound in the candidate record |

The earlier I02 leak boundary remains preserved as historical evidence at
`.codex-tmp/spec184-tiny-asan-20260911-i02/run.log` (SHA-256
`9171b9e06666a56fb5eae7d9309feabe804492b895dbebef1896860c2018b9d7`). This
repair closes the sampled I02 sanitizer ownership class only; I02–I08 process
and no-Python qualification, inherited negative rows, real-model/MiniNDN breadth,
Python retirement and external SIF/Tiger execution remain open.

The original `Spec175NativeTinyOnnx*` behavior-class batch was run in the
rebuilt ASan/UBSan tree with unsuppressed leak detection. It exited `134` because
LeakSanitizer reported `SUMMARY: AddressSanitizer: 3096985 byte(s) leaked in
25092 allocation(s)`; no use-after-free, buffer, or undefined-behavior report
preceded that leak summary. The raw first boundary is
`.codex-tmp/spec184-tiny-asan-20260911-r1/run.log`, SHA-256
`827b56f8c870f675fc7b8c9ee14113e743bc50199e966593c63ab069298da89f`.
It remains a historical `DYNAMIC_FAIL`; the repair and rerun below are the
current sanitizer result.

After the callback-cycle repair, the same independent ASan/UBSan tree reran the
16-case `Spec175NativeTinyOnnx*` behavior batch with unsuppressed leak detection.
All C++ positive and expected-negative assertions completed, process exit was
`0`, and the log ends with `*** No errors detected` with no ASan/UBSan/
LeakSanitizer diagnostics. Raw log: `.codex-tmp/spec184-tiny-asan-20260911-r3/run.log`,
SHA-256 `cc69c5f10ff39552d60a2eb0d107f56f9ce67dbe23937d8521f2cac86881630b`.
This is `DYNAMIC_PASS` for the sampled shared tiny-ONNX sanitizer class; rows
outside this selector remain governed by the qualification matrix.

The bounded ASan/UBSan I01 rerun (`Spec175NativeTinyOnnxI01OneProvider`)
exited `0` with eight events, final EOS, and no ASan/UBSan/LeakSanitizer
diagnostics; raw log `.codex-tmp/spec184-tiny-asan-20260911-r2/run.log`,
SHA-256 `34d9169715b04216a001050a54a85435062407de9726f4507198915d7cdde40c`.
The two-provider I02 case's earlier leak is the historical boundary addressed
above; the repaired selector and the repaired 16-case batch are both recorded
under the current candidate. No leak suppression was used.

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

## Parser-fuzz dynamic sample

The current C++ offer decoder now has a bounded deterministic parser-fuzz
selector, `Spec182ObservedOffer/Spec184NativeParserFuzz`.  It uses a fixed
seed and 512 mutations of the frozen canonical offer vectors, covering
truncation, byte replacement, insertion, suffix noise and structural prefix
damage.  The selector accepts only a bounded decoder rejection or a valid
decoded provider identity; it does not assign business meaning outside the
production decoder.  The candidate unit run and the independent unsuppressed
ASan/UBSan run both exited `0` with `*** No errors detected`.  The sanitizer
raw log is `.codex-tmp/spec184-parser-fuzz-20260911/run.log`, SHA-256
`2106e52606f7e82e04974d92c2872015a196f6fdfe02f095c4c01502e7a53d55`.
This closes the parser-fuzz sample for the observed-offer JSON risk class;
other inherited parser rows and the final candidate identity remain open.

## Review trace for the current diff

The read-only `review-agent` protocol was applied to the complete production
and parser-test diff against the current `Experimental` worktree.  The
reviewed paths were `NativeProviderHandler.cpp`, `NativeEpochCoordinator.cpp`
and `di-native-observed-offer.t.cpp`, including their call sites and test
registration.  Result: `No findings`.  The earlier I02 leak is retained as a
historical failure boundary; after the subsequent `ServiceProvider.cpp`
ownership repair, the current I02 sanitizer selector is clean.  Process/no-
Python breadth and external-owner boundaries remain qualification risks; they
are not promoted to PASS.

## Qualification decision

T007 remains `PARTIAL`: the fresh native unit/integration sweep, component dynamic
gates, observed-offer parser sample, repaired I02 sanitizer selector and repaired tiny-ONNX
sanitizer batch, and bounded fresh root `PO-001-stream` owner case pass, while I02–I08 process/no-Python breadth, inherited negative
collector rows, real-model/MiniNDN breadth, Python retirement and external
SIF/Tiger execution are not qualified.
T008 cannot start, and no promotion or final handoff is authorized by this record.
