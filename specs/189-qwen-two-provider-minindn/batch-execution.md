# Spec189 Batch Execution Register

This is the execution entrypoint for the five batches in [plan.md](plan.md). Every batch has one stable exit and one evidence record. The official read-only review-agent is required for each task snapshot and for the batch composition review before the shared build/test.

| Batch | Members | Stable exit | C++ selector / harness | Status | Evidence |
| --- | --- | --- | --- | --- | --- |
| B189-0 | T001 | source/model/ABI/split/handoff contract and candidate preflight frozen | CodeGraph + `rg`, `nm -C`, `readelf` map | PARTIAL | `evidence/b189-convergence.md` |
| B189-1 | T002,T003 | prepare commits reusable two-layer Repo reference | `DI_NativeArtifactAuthority` and Repo C++ selector | PARTIAL | `evidence/b189-prepare.md` |
| B189-2 | T004,T005 | ACK and signed two-provider Selection validated | `DI_NativeRequester` + Core controller | PARTIAL | `evidence/b189-placement.md` |
| B189-3 | T006,T007 | `GRANT_VERIFIED → EXECUTION_ENTERED → FETCH → ASSEMBLY → RUNNER_READY → EXECUTE → TERMINAL` | `di-native-provider`, `DI_NativeOnnxAssemblyWorker`, Spec189 C++ oracle | BLOCKED | `evidence/b189-execution.md` |
| B189-4 | T008 | resource guard and drain classify run | C++ counters + Python sampler | NOT_STARTED | `evidence/b189-resource.md` |
| B189-5 | T009,T010 | repeat evidence and verdict identity consistent | evidence checker + same native selectors | NOT_STARTED | `evidence/b189-convergence.md` |

## Dynamic gate card

Each batch evidence must freeze source/build/ABI/model/profile identity, output path, selector, repeat budget and resource floors. Host builds must use the installed global dependency closure documented in [`docs/native-dependency-closure.md`](../../docs/native-dependency-closure.md); temporary dependency prefixes are a preflight failure. Before `Run`, derive digests from the frozen candidate, validate policy/credential/interpreter closure, inspect the ONNX state contract and reject stale run-scoped publication residue. Run `Freeze → Preflight → Sample → Run → Classify`. Record static, compile/link, runtime/test and unobserved misses separately. For B189-3, `GRANT_VERIFIED` is not an execution exit; the provider-side marker sequence must identify the next boundary. A resource stop is not a protocol PASS.

## Batch growth decision

Stop at the stable exit. Do not add SIF/Tiger, general Repo redesign, broad generation quality or unrelated binding work to a batch. A changed caller, wire state, source closure or hard acceptance dependency starts a new batch and invalidates dependent evidence.

## Audit checkpoint

The r01-r21 audit found that static review could have caught the V3 endpoint
projection loss, service-scope policy mismatch, missing operator registry
closure, repeated manual digest hazards and missing preflight checks. The exact
r21 post-grant runtime boundary remains dynamic and is recorded in
[the static audit](evidence/spec189-static-audit-20260918.md). The next batch
must implement the missing post-grant markers and the C++ endpoint regression
before another full MiniNDN retry.

### Retry static-review gate

Each retry also follows the shared
[experiment static re-review loop](../../skills/speckit-code-design/references/experiment-static-review-loop.md):
preserve the prior raw attempt, name the first missing marker, declare the
Changed gate, freeze the complete candidate/diff snapshot, and obtain
read-only review-agent re-review before building or rerunning. If no real
caller, configuration, C++ fixture/oracle, build closure, resource guard or
negative-path check changed, the attempt remains `BLOCKED`/`PARTIAL`.
