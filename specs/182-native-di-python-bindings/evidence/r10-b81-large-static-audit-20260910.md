# Spec182 R10-B81 Large C++ Static Audit

Date: 2026-09-10
Branch: Experimental
Source baseline: `051660c4`
Decision: OPEN_FOR_NEXT_BATCH
Boundary: broad static review and local repair; no parent task advanced

## Scope and coverage

| Lane | Covered boundary |
| --- | --- |
| Production entry/callers | CodeGraph traced `NativeInferenceClient::request` → `dispatchOperation` → `beginCoreRequest` → Core `BeginCollaboration`; Python AST inventory covered 91 maintained files and the native YOLO branch. |
| Implementation/wire | Native requester/provider, `NativeInferenceClient`, `NativeEpochCoordinator`, `NativeRequestEnvelope`, `InvocationStream`, Waf source registration, state cleanup and conversation authority. |
| Test/harness/oracle | C++ integration and unit selectors, requester/provider examples, target registration, Cppcheck 1.90, Clang static analyzer and Python caller inventory. |
| Build/source closure | Current-source Waf integration and unit targets, `nm`/`readelf` symbol and registration checks, and candidate `RUNPATH`/`ldd` inspection. |
| Migration/evidence | 16 actual maintained public inference calls still use compatibility or automatic-planner APIs; the lone `.generate` match is `ProviderEvidenceSigner.generate()` and is not an inference caller. Independent worker/process transport, no-Python qualification and deployment closure remain open. |

## Static findings

- **AS-01 MEDIUM — CLOSED_FOCUSED.** `NativeInferenceClient.cpp` had an inner
  `if (!deferConversationCleanup)` that was unreachable under its enclosing
  condition. The capture and redundant branch were removed. Changed-file Cppcheck,
  Clang analyzer, the cancellation selector and the R10-B* integration sweep pass.
- **AS-02 LOW — CLOSED_FOCUSED.** `NativeEpochCoordinator.cpp` used an implicit
  range construction that Cppcheck 1.90 reported as a dangling lifetime. The
  explicit `std::vector<std::uint8_t>` range constructor removes the ambiguity;
  Clang analyzer reports no diagnostics and the original code was otherwise safe
  because the vector copies the string bytes.
- **AS-03 RETRACTED — CLOSED.** The earlier maximum-token mismatch is not a
  confirmed defect after tracing the direct path: `nativeGenerationFromOptions`
  caps `maxGeneratedTokens` at 64 before stream fields are populated.
- **AS-04 HIGH — OPEN.** The maintained inventory still contains 16 public
  inference calls on compatibility/automatic-planner APIs. Native helper
  coexistence does not close caller migration.
- **AS-05 HIGH — OPEN.** No independent requester → Core → Provider worker/process
  run has yet observed unary, stream, continuation/recovery and cleanup together.
  In-process R10-B* selectors do not satisfy this boundary.
- **AS-06 HIGH — OPEN.** The integration artifact `RUNPATH`/`ldd` closure still
  resolves NAC-ABE, ndn-svs and ONNX Runtime from host paths; T016/container
  source and deployment closure remains open.
- **AS-07 TOOL GAP.** Cppcheck 1.90 leaves 44 algorithm-style diagnostics and an
  `internalAstError` for a valid lambda, plus an intentional RAII unread variable.
  No confirmed product defect remains from the broad Cppcheck run after the
  focused Clang analyzer pass.

The bounded compatibility constructor path still intentionally returns
`NATIVE_REQUEST_PIPELINE_NOT_READY` when no complete runtime/preparation/encoding
chain is supplied. That is a fail-closed component boundary, not evidence that the
default constructor is a complete production request path; T010 remains open.

## Validation record

| Check | Result |
| --- | --- |
| Broad Cppcheck over 63 tracked C++ sources | exit 0 after repair; 50 low-value/style/tool diagnostics, no confirmed product error |
| Clang static analyzer on changed translation units | exit 0; 0 diagnostics |
| `./waf build --targets=integration-tests -j2` | PASS, 118/118, 22.21s elapsed, max RSS 1,239,996 KB, swaps 0 |
| `./waf build --targets=unit-tests -j2` | PASS, 188/188, 4m10.92s elapsed, max RSS 1,650,624 KB, swaps 0; one pre-existing warning at `tests/unit-tests/di-native-onnx-recipe.t.cpp:700` |
| Integration cancellation selector | PASS, 1 case, 5.53s |
| Integration `Spec182R10B*` sweep | PASS, 5 cases, 21.12s |
| Focused unit selectors | PASS, 30 cases across ClientState, Conversation and NativeInferenceClient |
| Full C++ `Spec182*` unit wildcard | PASS, 251 cases, 1m32.15s; max RSS 8,396,304 KB; no separate `vmstat` sample captured |
| Source and target closure | `nm`/`readelf` and Waf registration show changed symbols and translation units are linked; runtime dependency paths remain an open deployment boundary |

## Review trace and batch decision

The review used the official `$review-agent` instructions at
`/home/tianxing/.codex/skills/review-agent/SKILL.md` (SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`) together
with the project pre-test static review reference (SHA-256
`f59753415869168ee469239ebcb12b546101375d976bac7061e834062e8556c6d`) and
batch gates (SHA-256
`e8a32b934b22b82e5a502db02aecf6b8b849908cf9499cae1b51531f7f69a126`). The
review baseline was `051660c4`; coverage included CodeGraph call-path queries,
`rg`/Python AST caller inventory, Cppcheck, Clang analyzer, Waf registration,
`nm` and `readelf`.

`STATIC_PASS`, `BUILD_PASS` and `FOCUSED_BEHAVIOR_PASS` apply only to the local
cleanup boundary. The batch is `OPEN_FOR_NEXT_BATCH`: stop here rather than
expanding the batch into worker transport, caller migration or qualification.
The next stable exit is one fresh independent requester/Provider process case
with artifact identity and source/dependency closure recorded. Parent tasks
T010/T011/T013/T014/T015/T016/T017 remain PARTIAL or NOT_STARTED.

