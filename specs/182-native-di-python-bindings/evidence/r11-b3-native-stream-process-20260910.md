# R11-B3 Native Stream Process Evidence

**Date**: 2026-09-10
**Status**: \`CLOSED_FOR_VALIDATION\` for the bounded independent native C++ stream process
**Source checkpoint**: \`3094477f\` plus the uncommitted R11-B3 source unit recorded below

本批验证的是独立 C++ requester → Core → Provider worker 的真实 token stream 出口。Python
只负责生成私有 NFD/PIB/TPM、catalog、plan、manifest 和进程生命周期；请求编码、grant
验证、Provider 装配、ORT 执行、事件接收、最终 payload 与 token oracle 均由 C++ 生产程序完成。
本证据不推进 T010/T011/T013/T016，也不代表 continuation、recovery、replacement 或
no-Python 资格已经完成。

## Command and artifacts

\`\`\`text
python3 tests/standalone/run-spec182-native-stream-process.py \
  --build .codex-tmp/spec182-r11-b2-fresh-20260910/build
\`\`\`

Final raw run: \`/tmp/spec182-r11-b3-probe-vulbu5rz\`
Build directory: \`.codex-tmp/spec182-r11-b2-fresh-20260910/build\`
Requester SHA-256: \`75628c23d030469e05fba5937f961f9673e50700688df1a7f251531a98637b78\`
Provider SHA-256: \`09ba35688484b13281232d7699909fdbc03915ea4182103482ca0272c029de7e\`
ONNX fixture SHA-256: \`fe41db5c2c8397b62fc8983307b0d755706b7008c56958882cb247ca46594f82\`
Standalone tokenizer SHA-256: \`bf0f0fa65dc5aafe690ee497b4c8e2abe408fb4788c5ef035964dc94f15ca5a6\`

## Observed PASS markers

\`requester.log\` contains:

\`\`\`text
NATIVE_STREAM_ORACLE_PASS tokens=4,5,6,7,8,9,10,2 events=8
NATIVE_REQUEST_SUCCEEDED request=/NDNSF/DI/REQUEST/<run-specific>-1 plan=sha256:<run-specific>
\`\`\`

\`provider.log\` contains \`NDNSF_DI_GRANT_VERIFICATION\` at \`BEFORE_ASSEMBLY\` and
\`NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED\` with \`runnerKind=onnxruntime-cpu\`,
\`realCompute=true\`, \`runtimeVersion=1.26.0\`, \`device.kind=cpu\`,
\`executionCompleted=true\`, and all certified role node assignments.

The same run therefore demonstrates ordered eight-event delivery, exact C++ token oracle,
terminal final response, authenticated grant verification, post-selection runner preparation,
decode-state commit across epochs, and real CPU ONNX Runtime execution in separate processes.

## Source changes exercised

- \`NativeProviderRuntime::executePreparedRoleAsync\` now uses the same stateful staging,
  predecessor transition, commit, rollback and cleanup path as ordinary runners, while
  accepting a post-Selection preparation callback.
- Dynamic prepared runners can derive exact decode-state bindings from the sealed identity when
  a metadata-only runner slot is intentionally absent.
- Qwen split projections carry optional ingress/egress roles; the catalog passes them through,
  and the ONNX assembler retains certified selected nodes that are side-effect-free but not
  reachable from the ordinary output traversal.
- Prepared runner metadata carries the authenticated state input/output names so each epoch
  supplies and commits the declared decode state.
- Generation event prefix digests use the project-wide lowercase \`sha256:\` encoding, and
  repeated preparation status updates use a monotonic operation sequence.

## Preserved failed boundaries

Earlier raw runs remain under \`/tmp/spec182-r11-b3-probe-*\` and are not overwritten. The first
failure in each family was recorded before retry:

- missing requester options/tokenizer configuration;
- metadata-only runner validation and missing ingress/egress wiring;
- token input discovery and state metadata absence;
- invalid alternate ONNX fixture graph/type contract;
- decode-state candidate/commit bypass in the prepared-runner path;
- uppercase event digest rejected by the C++ requester;
- repeated readiness status rejected as stale.

These failures are implementation or harness contract boundaries, not successful protocol
runs. The final run is the first one with all required C++ stream markers.

## Verification summary

- C++ \`Spec182*\` unit selector: **256/256 cases, 7077/7077 assertions PASS**.
- C++ \`Spec170NdnsfDiCoreFlow/Spec182*\` integration selector: **9/9 cases, 55/55 assertions PASS**.
- Independent process driver: **exit 0**, eight token events and final C++ oracle PASS.
- \`python3 -m py_compile tests/standalone/run-spec182-native-stream-process.py\`: PASS.
- \`git diff --check\`: PASS before the documentation checkpoint.

Remaining gates are the next R11 cards: continuation, recovery, replacement, cleanup,
maintained callers, no-Python dependency exclusion, convergence audit and final qualification.
