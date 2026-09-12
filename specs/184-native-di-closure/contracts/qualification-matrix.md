# Spec184 Qualification Matrix

**Status**: PARTIAL / T006 row binding complete; fresh local candidate is bound for qualification and open rows remain
**Source**: [transfer matrix](transfer-matrix.md), inherited Spec182 proof contracts, and the fresh local candidate recorded in the promotion contract

The `candidateId` values in the historical row tables retain their original development
checkpoints for traceability. The fresh candidate overlay below is the only current binding for
the 2026-09-11 qualification attempt; row status remains `PARTIAL` until that row's complete
selector or external-owner evidence is available.

本表是最终资格的唯一行级入口。迁移记录的结构检查不能关闭任何一行；每行必须绑定
当前 candidate、源码/二进制身份、真实 C++ target/selector 或外部 owner、负例、所有子
进程 exit、cleanup 和 evidence path。`TRANSFERRED` 只表示归属，`PASS` 只允许来自当前
candidate 的完整结果。

## Inherited Parent Obligations

| Row | Inherited obligation | Spec184 owner | Required evidence | Dynamic profile / invariant | Initial status |
| --- | --- | --- | --- | --- | --- |
| 182:T004 | canonical seal/非法投影与 Core/Provider 真实接收资格 | T006/T007 | C++ `Spec182PlanSealer/*` selectors、wire/source identity、负例结果 | `asan-ubsan`; canonical bytes and rejection cleanup | PARTIAL |
| 182:T005 | grant IO 缺陷 F-01、签发/验证/消费完整资格 | T001/T006/T007 | `Spec184AuthorityIoOwnership`、真实 authority/provider 结果 | `tsan`; IO owner and pending-call balance | PARTIAL |
| 182:T006 | cold ONNX 装配 bytes/取消/清理、Python helper 退出验收 | T006/T007 | `Spec182NativeAssembly/*`、worker target、process exit and cleanup | `asan-ubsan`; runner lifetime and residue=0 | PARTIAL |
| 182:T007 | tokenizer encode/decode oracle 与无解释器依赖 | T006/T007 | `Spec182NativeTokenizer/*`, `Spec182TokenizerFull/*`、dependency/source closure | `parser-fuzz`; malformed wire bounded failure | PARTIAL |
| 182:T008 | 原生输入/模型/工件准备与 admission 完整验收 | T006/T007 | `Spec182Preparation/*`, `Spec182OfferAdmission/*`、artifact-bound result | `asan-ubsan`; artifact ownership and cleanup | PARTIAL |
| 182:T009 | 共用 Provider host 注册、执行、停止与入口一致性 | T005/T006/T007 | `Spec182ProviderHost/*`, `Spec182Registration/*`, `Spec182SharedLease/*` | `asan-ubsan`; stop/cleanup and host resolver lifetime | PARTIAL |
| 182:T010 | F-01/F-02、完整 request lifecycle/负例 | T001/T002/T006/T007 | B1 selectors、`Spec182NativeInferenceClient/*`, 交错/取消/晚回调结果 | `tsan`; request state and callback fencing | PARTIAL |
| 182:T011 | F-03/F-04、continuation/受限恢复/lineage 剩余资格 | T003/T004/T006/T007 | `Spec184DurableOutcome`, `Spec184NativeCheckpoint/*`, conversation selectors | `asan-ubsan`; journal/checkpoint lifetime | PARTIAL |
| 182:T012 | 薄绑定支持模式，无 Python strategy/state owner | T005/T006/T007 | caller matrix、C++ owner selectors、binding/source closure | `none` for static closure; runtime row profile as applicable | PARTIAL |
| 182:T013 | 维护默认路由与 legacy retirement | T005/T006/T007 | caller matrix、default route、zero-use/rollback evidence | `none` with explicit caller boundary | PARTIAL |
| 182:T014 | no-Python harness/collector、依赖排除和负例 | T006/T007 | C++ process/no-Python owner、I02–I08 harness selectors | `asan-ubsan`; child cleanup and bounded exit | PARTIAL |
| 182:T015 | FR/CD/INV/PO、接线、oracle、build、设计总对账 | T006 | 80-row matrix、component evidence、fresh convergence audit | `none`; documentation/matrix invariants only | PARTIAL |
| 182:T016 | 同源完整 unit/integration、MiniNDN/no-Python 本地资格 | T007 | candidate-bound complete results and child exits | per row profile; terminal cleanup and identity binding | OPEN |
| 182:T017 | 唯一交付基线、维护文档、两入口示例与外部边界 | T008 | final source/bundle identity and handoff | `none`; evidence/status agreement | OPEN |

## Required Additional Rows

T006 必须在本表追加一行对应每个 `PO-001`–`PO-016`，以及适用的 `I`, `FR`, `CD`,
`INV`。不得使用 `PO-*` 或 `I-*` 通配行代替实际行。新增行至少包含：

`rowId`, `sourceContract`, `ownerTask`, `productionEntry`, `C++ target/selector or external owner`,
`negative/recovery boundary`, `riskClass`, `dynamicProfile`, `dynamicInvariant`, `candidateId`,
`source/runtime/config hashes`, `evidencePath`, `status`。

## Fresh Candidate Overlay

The current local candidate is recorded in the [promotion candidate contract](promotion-candidate.md)
and [fresh convergence audit](../evidence/convergence-b5.md). Its identity is intentionally kept in
that single candidate record so the matrix hash does not form a circular digest dependency.
The fresh candidate provides complete native unit and integration exits (`0`) and the observed-offer
parser sample (`Spec182ObservedOffer/Spec184NativeParserFuzz`, `512` fixed-seed mutations). These
results update evidence availability for the affected rows but do not upgrade them:

| Current evidence | Rows informed | Current boundary | Status |
| --- | --- | --- | --- |
| Fresh unit/integration target closure, candidate-first libraries, explicit worker lookup and root owner `PO-001-stream` | 182:T006, 182:T016, PO-001, PO-005, PO-011, PO-015/016, FR-006/012/013/014, CD-005/009/011/012, INV-007/009 | non-root owner probe stops at `MININDN_REQUIRES_ROOT`; I02–I08 and external SIF/Tiger not run | PARTIAL/OPEN |
| Observed-offer C++ parser sample and unsuppressed ASan/UBSan run | 182:T007, PO-006, FR-007, CD-006, INV-004 | tokenizer-full, plan/parser and no-Python negative rows remain unqualified | PARTIAL |
| Repaired C++ I02 two-provider selector and 16-case tiny-ONNX batch under unsuppressed ASan/UBSan | 182:T011, PO-008, FR-008/016, CD-007 | callback-cycle leak no longer reproduces; process/no-Python I02–I08, inherited negative and real-model rows remain open | PARTIAL |
| Fresh C++ production path review | PO-001, PO-013, FR-001/016, CD-013 | no independent process/no-Python owner result for current candidate; broader model rows open | PARTIAL |
| Current-candidate C++ process refresh: unary/stream/conversation/recovery/replacement/grant, no-Python ELF closure, root `PO-001-stream` owner, and fresh YOLO Y-A/Y-B/Y-N | 182:T010, 182:T011, 182:T014, 182:T016, PO-001, PO-004, PO-007/008, PO-011/013/014/016, I01/I06/I07/I08, FR-001/005/008/012/013/014/016/019, CD-001/004/007/009/011/012/013/014, INV-002/005/006/007/009 | bounded C++ process classes, owner evidence and YOLO native request rows pass; Y-N covers all seven boundaries and three Provider grant mutations; non-root owner preflight is `MININDN_REQUIRES_ROOT`; Qwen3.6-27B cannot execute on this host (Qwen3-0.6B is smoke-only); broader negative/model/Python-retirement/external rows remain open | PASS_FOR_ROWS / PARTIAL |
| Candidate-bound C++ I02–I08 dynamic process samples: two/four-provider planning, reorder, duplicate, loss/retry, negative retention, and cancellation | 182:T010, 182:T011, 182:T014, 182:T016, I02/I03/I04/I05/I06/I07/I08, FR-008/014/016/019, CD-007/009/011/014, INV-002/005/006/007/009 | all seven root owner runs have complete observation and seven evidence categories with C++ markers; broader real-model breadth, Python retirement, and external SIF/Tiger remain open | PASS_FOR_DYNAMIC_SAMPLE / PARTIAL |
| Candidate-bound C++ isolation counterexamples I02–I08 | 182:T014, 182:T016, I02/I03/I04/I05/I06/I07/I08, INV-007/009 | dedicated C++ fixture and canonical collector match the frozen statuses: I02/I03/I04/I06/I08 `FAIL`, I05 `UNQUALIFIED`, I07 `PASS`; I05 is intentionally not promoted, and real-model breadth, retirement and external owner rows remain open | PASS_FOR_COUNTEREXAMPLE / PARTIAL |
| Candidate six business binaries ELF/no-Python closure (`readelf -d`, `strings`, `ldd -r`) | 182:T006, 182:T014, 182:T016, PO-001/005/011/014, FR-006/012/014, CD-005/009/011/014, INV-007 | no forbidden Python dependency or runtime identity and no unresolved symbol; runtime isolation and external owner rows still require their declared process evidence | PASS_FOR_ROW / PARTIAL |

## Remaining Qualification Gates

以下出口按依赖执行，详细首边界与日志见 [T007 remainder audit](../evidence/remainder-audit-20260911.md)。
状态只表示当前证据，不得把 `0.6B` smoke 或输入 preflight 当作真实模型资格：

| Gate | Status | Required closure |
| --- | --- | --- |
| `T007-A0` current-candidate runtime receipt | `PASS_FOR_ROW` | `build-spec184-b5-candidate-r4/spec180-native-build.json`, `verify` exit `0`, `/usr/bin/g++ -B/usr/bin`, `-j4`, and candidate-first paths; receipt records `binding_reused=true` |
| `T007-A1` YOLO26n Y-A | `PASS_FOR_ROW` | Fresh current-candidate root run `r51`, C++ numerical oracle, terminal result, child exits and cleanup |
| `T007-A2` YOLO26n Y-B/Y-N | `PASS_FOR_ROW` | Y-B `r37` and Y-N `r50`; protected multi-provider terminal path, seven declared Y-N boundaries, three Provider grant mutations, first-failure classification and cleanup |
| `T007-A3` Qwen3.6-27B | `WAITING_EXTERNAL_INPUT` | Exact `Qwen/Qwen3.6-27B` three-stage manifest, tokenizer, CUDA runtime and experiment-owner result; local 0.6B is smoke-only |
| `T007-A4` inherited negative/retirement rows | `PARTIAL` | Complete row evidence, including I05 collector boundary or an explicit retained `UNQUALIFIED` result |
| `T008` native development handoff | `BLOCKED_BY_T007` | T007 qualification pass, final candidate map and external rows explicitly marked `TRANSFERRED` |

## PO Closure Rows

以下是当前逐项盘点。`DEV-865e1ee2` 只是本地开发 checkpoint 的关联标签，尚未成为
promotion candidate；`PARTIAL`/`OPEN` 不得被 T007 当作资格通过。

| rowId | sourceContract | ownerTask | productionEntry / C++ target or external owner | negative/recovery boundary | riskClass / dynamicProfile / dynamicInvariant | candidateId / source-runtime-config identity | evidencePath | status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| PO-001 | 182 proof-design PO-001 / FR-001/012 / CD-001/009 | T006/T007 | installed C++ consumer → native requester/provider; T016 owner for process run | interpreter or DI Python dependency appears; child/trace incomplete | isolation / `asan-ubsan` / no Python, bounded child cleanup | `DEV-865e1ee2`; source checkpoint, runtime hash per run, config `UNFROZEN` | 182 R11-B9-G7 current stream evidence; B5 rerun required | PARTIAL |
| PO-002 | 182 proof-design PO-002 / FR-003/009 / CD-002 | T006/T007 | `NativeModelSplitStrategy` + `NativePlacementStrategy`; `Spec182NativePlanning/QwenLayerSplitProducesCanonicalRankOneCandidate`, `YoloComponentSplitIsDeterministicAndBudgetTruncates`, `PreSplitPlacementFiltersAndDeterministicallyBindsOneProvider`, `PlacementAssignsDistinctRoleSpecificProvidersAndSeals` | invalid rank/device/lease/cut vector | planning / `none` / legal cut and deterministic order | `DEV-76072468`; unit hash `7a8375e4...`, runtime/config `UNFROZEN` | [B5 component evidence](../evidence/b5-component-validation-20260911.md); planning selector log `.codex-tmp/spec184-b5-candidate-focused-20260911/` | PARTIAL |
| PO-003 | 182 proof-design PO-003 / FR-002/004 / CD-003 | T006/T007 | `NativePlanSealer` → Core `CommitCollaborationPlan`; `Spec182PlanSealer/PlanSealerBindsSnapshotArtifactsAndGrantContext`, `PlanSealerRejectsIncompleteSubstitutedOrOutsideGrantCover`, `SealCoreRejectsForeignArtifactsAndInexactCover` | missing endpoint, ACK digest mismatch, duplicate commit | serialization / `asan-ubsan` / canonical bytes and independent parser agreement | `DEV-76072468`; unit hash `7a8375e4...`, runtime/config `UNFROZEN` | [B5 component evidence](../evidence/b5-component-validation-20260911.md); plan-sealer selector log `.codex-tmp/spec184-b5-candidate-focused-20260911/` | PARTIAL |
| PO-004 | 182 proof-design PO-004 / FR-005 / CD-004 | T001/T006/T007 | C++ requester → independent authority → verifier | expired, wrong recipient, forged signature, key bypass | authorization / `tsan` / IO owner and grant binding | `DEV-865e1ee2`; B1 runtime hashes, authority config not frozen | B1 evidence; independent process qualification pending | PARTIAL |
| PO-005 | 182 proof-design PO-005 / FR-006/016 / CD-005 | T006/T007 | `NativeProviderHost` selection → assembler; `Spec182NativeAssembly/AssemblesComponentSetWithoutInterpreter`, `Spec182NativeAssembly/RejectsDuplicateNodeCover`, `Spec182ProviderHost/Spec182ProviderHostFixedLeaseEntryRoutesRealDispatch` | recipe/node/path/ciphertext/key mismatch; worker timeout | assembly/lifetime / `asan-ubsan` / certified artifact ownership and residue zero | `DEV-76072468`; unit hash `7a8375e4...`, worker hash `336ce668...`, runtime/config `UNFROZEN` | [B5 component evidence](../evidence/b5-component-validation-20260911.md); ASan Provider-host log `.codex-tmp/spec184-b5-provider-fix2-asan-20260911.log` | PARTIAL |
| PO-006 | 182 proof-design PO-006 / FR-007 / CD-006 | T006/T007 | native tokenizer adapter and C++ decoder; `Spec182NativeTokenizer/MissingArtifactIsRejectedBeforeEngineCreate`, `DigestMismatchIsRejectedBeforeEngineCreate`, `StandaloneFactoryRequiresIdentity`, `Spec182TokenizerFull/*` | digest mismatch, Unicode/special/byte-fallback error, Python helper | parser/lifetime / `parser-fuzz` / exact text and bounded malformed-input failure | `DEV-76072468`; unit hash `7a8375e4...`, tokenizer artifact/config `UNFROZEN` | [B5 component evidence](../evidence/b5-component-validation-20260911.md); tokenizer selector log `.codex-tmp/spec184-b5-candidate-focused-20260911/` | PARTIAL |
| PO-007 | 182 proof-design PO-007 / FR-008 / CD-001/007 | T001/T002/T006/T007 | `NativeInferenceClient` request/stream lifecycle | cancel/deadline/late callback/close/revival | concurrency / `tsan` / one terminal state and no stale callback | `DEV-865e1ee2`; B1 TSan and B2 runtime hashes | B1/B2 evidence; process cancellation rows pending | PARTIAL |
| PO-008 | 182 proof-design PO-008 / FR-008/016 / CD-007 | T006/T007 | native Qwen conversation/continuation owner | wrong parent, duplicate prefix, replacement fencing | continuation / `asan-ubsan` / lineage and prefix monotonicity | `DEV-865e1ee2`; Qwen artifact/config `UNFROZEN` | B4 Qwen focused selectors; complete token oracle pending | PARTIAL |
| PO-009 | 182 proof-design PO-009 / FR-010 / CD-008 | T006/T007 | C++ consumer and optional Python facade | Python strategy callback or input mismatch | binding / `none` / native and facade semantics agree without callback execution | `DEV-865e1ee2`; extension/runtime/config `UNFROZEN` | B4 38-test route regression; full parity matrix pending | PARTIAL |
| PO-010 | 182 proof-design PO-010 / FR-011/016 / CD-010 | T005/T006/T007 | maintained caller rows and legacy exclusion checks | old provider/coordinator/helper reconnected | routing / `none` / explicit native default and zero-use observation | `DEV-865e1ee2`; caller matrix source `865e1ee2`, runtime/config `UNFROZEN` | caller matrix and B4 evidence | PARTIAL |
| PO-011 | 182 proof-design PO-011 / FR-013/014 / CD-011 | T006/T007 | C++ unit/integration/process plus MiniNDN owner | startup/collector/secret/exit failure misreported as protocol result | qualification / inherited row profiles / candidate identity and terminal cleanup | `DEV-865e1ee2`; no promoted candidate | 182 case manifest; full current matrix not run | OPEN |
| PO-012 | 182 proof-design PO-012 / FR-014/015 / CD-012 | T006/T008 | clean checkout and handoff receiver | source/dependency/model/config/harness mismatch | delivery / `none` / ordered manifest identity and preflight refusal | `DEV-865e1ee2`; source checkpoint only, bundle/config `UNFROZEN` | promotion candidate contract | OPEN |
| PO-013 | 182 proof-design PO-013 / FR-001/002/004/009/016 / CD-013 | T006/T007 | native `prepareInput`/`inspectModel`/`ensureArtifacts`/`verify` | wrong provenance, catalog/publication name or candidate | provenance / `asan-ubsan` / request/artifact/model identity binding | `DEV-865e1ee2`; source `865e1ee2`, artifacts/config `UNFROZEN` | B4 route/assembly selectors; full preparation matrix pending | PARTIAL |
| PO-014 | 182 proof-design PO-014 / FR-001/009/010/012 / CD-014 | T005/T006/T007 | native consumer/CLI/binding → shared Provider host; `Spec182ProviderHost/*`, `Spec182Registration/*`, `Spec182SharedLease/*` | old Python runner, early handler destruction, unrelated service stop | host/lifetime / `asan-ubsan` / registration generation, bounded stop, no resolver leak | `DEV-76072468`; unit hash `7a8375e4...` | [B5 component evidence](../evidence/b5-component-validation-20260911.md); normal and unsuppressed ASan/UBSan Provider-host logs | PARTIAL |
| PO-015 | 182 proof-design PO-015 / FR-018 / CD-001--014 | T006 | review-agent plus Spec184 convergence audit | missing caller, test registration, source closure or design contradiction | review / `none` / five lanes complete and findings dispositioned | `DEV-76072468`; ordered local candidate and source closure recorded in promotion contract | B1–B4 review traces; [B5 component review](../evidence/b5-component-validation-20260911.md); [B5 convergence audit](../evidence/convergence-b5.md) | PASS |
| PO-016 | 182 proof-design PO-016 / FR-019 / CD-001--014 | T007/T008 | T007 complete run and T008 handoff | any required row unrun or unbound | qualification / per row / all required local results share candidate identity | `DEV-865e1ee2`; no promoted candidate | no final qualification record | OPEN |

## Isolation Rows

These rows preserve the eight inherited no-Python/collector counterfactuals. They are external
owner cases, not substitutes for native C++ behavior selectors; Python only operates the harness.

| rowId | sourceContract | ownerTask | productionEntry / C++ target or external owner | negative/recovery boundary | riskClass / dynamicProfile / dynamicInvariant | candidateId / source-runtime-config identity | evidencePath | status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| I01 | 182 native-isolation-design I01 | T006/T007 | canonical installed native consumer; MiniNDN owner | valid native consumer must complete with full evidence | isolation / `none` / marker, namespace, trace and cleanup complete | `DEV-865e1ee2`; installed consumer identity pending | 182 R11-B9-G7 / I01 evidence | PARTIAL |
| I02 | 182 native-isolation-design I02 | T006/T007 | canonical runner with fork-helper and renamed Python ELF counterfactual | exec/source identity violation rejected before business result | isolation / `asan-ubsan` / rejection boundary and no fallback | `owner.result=sha256:e65ee9154e51e2e950e7676b5f4efb467054b51add12c5a041dd404565eb0514`; fixture/runner identity in [T007 process evidence](../evidence/t007-process-qualification-20260911.md); candidate identity in [promotion candidate](promotion-candidate.md) | C++ owner `FAIL` / `UNDECLARED_EXEC`; Python selector remains regression coverage | PARTIAL |
| I03 | 182 native-isolation-design I03 | T006/T007 | runtime mapping detector | transient or renamed `libpython` mapping fails | isolation / `asan-ubsan` / complete mapping observation | `owner.result=sha256:e65ee9154e51e2e950e7676b5f4efb467054b51add12c5a041dd404565eb0514`; fixture/runner identity in [T007 process evidence](../evidence/t007-process-qualification-20260911.md); candidate identity in [promotion candidate](promotion-candidate.md) | C++ owner `FAIL` / `PYTHON_MAPPING`; Python selector remains regression coverage | PARTIAL |
| I04 | 182 native-isolation-design I04 | T006/T007 | endpoint detector plus native requester/provider | host TCP/abstract UNIX/undeclared socket rejected while NFD allowlist works | isolation / `tsan` / declared endpoint-only communication | `owner.result=sha256:e65ee9154e51e2e950e7676b5f4efb467054b51add12c5a041dd404565eb0514`; fixture/runner identity in [T007 process evidence](../evidence/t007-process-qualification-20260911.md); candidate identity in [promotion candidate](promotion-candidate.md) | C++ owner `FAIL` / `UNDECLARED_ENDPOINT`; Python selector remains regression coverage | PARTIAL |
| I05 | 182 native-isolation-design I05 | T006/T007 | trace and child collector | missing trace tail/short-lived child/observer kill is `UNQUALIFIED` | evidence / `none` / observation completeness and bounded result | `owner.result=sha256:e65ee9154e51e2e950e7676b5f4efb467054b51add12c5a041dd404565eb0514`; fixture/runner identity in [T007 process evidence](../evidence/t007-process-qualification-20260911.md); candidate identity in [promotion candidate](promotion-candidate.md) | C++ owner `UNQUALIFIED` / `TRACE_BUDGET_EXCEEDED`; Python selector remains regression coverage | PARTIAL |
| I06 | 182 native-isolation-design I06 | T006/T007 | cold native requester/provider process | warm-only model, harness-generated plan/text, or missing Provider fails | assembly / `asan-ubsan` / cold preparation, role coverage and independent oracle | `owner.result=sha256:e65ee9154e51e2e950e7676b5f4efb467054b51add12c5a041dd404565eb0514`; fixture/runner identity in [T007 process evidence](../evidence/t007-process-qualification-20260911.md); candidate identity in [promotion candidate](promotion-candidate.md) | C++ owner `FAIL` / `ROLE_COVERAGE_MISMATCH`; Python selector remains regression coverage | PARTIAL |
| I07 | 182 native-isolation-design I07 | T006/T007 | external Python harness with native business processes | harness Python is allowed; business owner must remain no-Python | isolation / `none` / process/library allowlist and owner evidence | `owner.result=sha256:e65ee9154e51e2e950e7676b5f4efb467054b51add12c5a041dd404565eb0514`; fixture/runner identity in [T007 process evidence](../evidence/t007-process-qualification-20260911.md); candidate identity in [promotion candidate](promotion-candidate.md) | C++ owner `PASS`; Python remains harness only | PARTIAL |
| I08 | 182 native-isolation-design I08 | T006/T007 | process-group and descendant collector | detached/late child leaves no owned process or becomes `UNQUALIFIED` | lifetime / `tsan` / PID lineage and cleanup completeness | `owner.result=sha256:e65ee9154e51e2e950e7676b5f4efb467054b51add12c5a041dd404565eb0514`; fixture/runner identity in [T007 process evidence](../evidence/t007-process-qualification-20260911.md); candidate identity in [promotion candidate](promotion-candidate.md) | C++ owner `FAIL` / `OWNED_PROCESS_ALIVE`; Python selector remains regression coverage | PARTIAL |

## Functional and Design Crosswalk

每个继承 ID 单独列出，避免用范围或通配符掩盖缺口。`sourceContract` 指向 Spec182 原
契约；`entry/selector` 可在 T006/T007 继续替换为当前同源 C++ 选择器。

| rowId | sourceContract | ownerTask | entry / selector | boundary | profile / invariant | candidateId | evidencePath | status |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| FR-001 | 182 spec FR-001 | T001/T006/T007 | NativeInferenceClient + native consumer | missing authority/preparation or wrong source | `tsan`; owner and provenance binding | `DEV-865e1ee2` | B1 evidence; PO-004/013 | PARTIAL |
| FR-002 | 182 spec FR-002 | T006/T007 | NativePlanSealer; `Spec182PlanSealer/PlanSealerBindsSnapshotArtifactsAndGrantContext`, `PlanSealerEncodeRejectsIncompleteProjections` | missing/changed canonical field | `asan-ubsan`; canonical bytes | `DEV-76072468` | PO-003; [B5 component evidence](../evidence/b5-component-validation-20260911.md) | PARTIAL |
| FR-003 | 182 spec FR-003 | T006/T007 | NativeModelSplitStrategy; `Spec182NativePlanning/QwenLayerSplitProducesCanonicalRankOneCandidate`, `YoloComponentSplitIsDeterministicAndBudgetTruncates` | illegal graph cut or budget | `none`; deterministic legal proposal | `DEV-76072468` | PO-002; [B5 component evidence](../evidence/b5-component-validation-20260911.md) | PARTIAL |
| FR-004 | 182 spec FR-004 | T006/T007 | Core plan commit/parser; `Spec182PlanSealer/PlanSealerRejectsTamperedFrozenCoreAndSecurity`, `SealCoreRejectsForeignArtifactsAndInexactCover` | duplicate or tampered plan | `asan-ubsan`; independent parser agreement | `DEV-76072468` | PO-003; [B5 component evidence](../evidence/b5-component-validation-20260911.md) | PARTIAL |
| FR-005 | 182 spec FR-005 | T001/T006/T007 | NativeGrantClient/Verifier | wrong signer, recipient or expiry | `tsan`; grant binding and pending balance | `DEV-865e1ee2` | B1 evidence; PO-004 | PARTIAL |
| FR-006 | 182 spec FR-006 | T006/T007 | NativeCanonicalOnnxAssembler; `Spec182NativeAssembly/AssemblesComponentSetWithoutInterpreter`, `RejectsDuplicateNodeCover` | recipe/path/key mismatch | `asan-ubsan`; artifact ownership | `DEV-76072468` | PO-005; [B5 component evidence](../evidence/b5-component-validation-20260911.md) | PARTIAL |
| FR-007 | 182 spec FR-007 | T006/T007 | NativeTokenizer/decoder; `Spec182NativeTokenizer/*`, `Spec182TokenizerFull/*` | malformed ids or Python helper | `parser-fuzz`; exact text and bounded error | `DEV-76072468` | PO-006; [B5 component evidence](../evidence/b5-component-validation-20260911.md) | PARTIAL |
| FR-008 | 182 spec FR-008 | T001/T002/T006/T007 | NativeInferenceClient lifecycle | cancel/deadline/late callback | `tsan`; one terminal result | `DEV-865e1ee2` | B1/B2 evidence; PO-007/008 | PARTIAL |
| FR-009 | 182 spec FR-009 | T005/T006/T007 | YOLO/Qwen native adapters | model branch or role identity mismatch | `none`; shared mechanism and model-specific adapter | `DEV-865e1ee2` | caller matrix; PO-002/013 | PARTIAL |
| FR-010 | 182 spec FR-010 | T005/T006/T007 | binding facade and C++ consumer | callback or input parity mismatch | `none`; no Python strategy owner | `DEV-865e1ee2` | B4 Python regression; PO-009 | PARTIAL |
| FR-011 | 182 spec FR-011 | T005/T006/T007 | maintained caller/legacy exclusion | old default path reconnected | `none`; explicit default and zero-use | `DEV-865e1ee2` | caller matrix; PO-010 | PARTIAL |
| FR-012 | 182 spec FR-012 | T006/T007 | native library/provider/requester targets | stale or missing shared library | `asan-ubsan`; source/runtime closure | `DEV-865e1ee2` | B3/B4 build records; PO-001/014 | PARTIAL |
| FR-013 | 182 spec FR-013 | T006/T007 | C++ selectors and qualification owner | startup/collector failure misclassified | per row; independent result and cleanup | `DEV-865e1ee2` | B1–B4 evidence | PARTIAL |
| FR-014 | 182 spec FR-014 | T007/T008 | candidate and handoff bundle | changed source/config/artifact | `none`; ordered identity | `DEV-865e1ee2` | promotion candidate contract | OPEN |
| FR-015 | 182 spec FR-015 | T006 | baseline and dependency gate | historical status rewritten | `none`; baseline preservation | `DEV-865e1ee2` | transfer matrix | PARTIAL |
| FR-016 | 182 spec FR-016 | T006/T007 | shared native capabilities | relocation drops behavior | per row; capability parity | `DEV-865e1ee2` | PO-005/008/013 | PARTIAL |
| FR-017 | 182 spec FR-017 | T006/T008 | design/source/symbol documentation | missing ownership or usage explanation | `none`; source-to-contract trace | `DEV-865e1ee2` | B4 matrix/review trace | PARTIAL |
| FR-018 | 182 spec FR-018 | T005/T006 | review-agent static gates | missing lane, test registration or source map | `none`; five-lane coverage | `DEV-865e1ee2` | B1–B4 evidence | PARTIAL |
| FR-019 | 182 spec FR-019 | T006/T007 | native-first verification order; `tasks.md` Execution Progress and `Logical Batch Progress` | later phase hides earlier gap | per row; phase exit is explicit | `DEV-76072468` | [task progress registry](../evidence/task-progress-registry-20260911.md); [B5 component evidence](../evidence/b5-component-validation-20260911.md) | PARTIAL |
| CD-001 | 182 code-design CD-001 | T006/T007 | NativeInferenceClient public API | invalid input/options/close | `tsan`; ownership and error mapping | `DEV-865e1ee2` | B1 evidence; PO-007 | PARTIAL |
| CD-002 | 182 code-design CD-002 | T006/T007 | split/placement strategies; `Spec182NativePlanning/*` | illegal proposal | `none`; deterministic legal output | `DEV-76072468` | PO-002; [B5 component evidence](../evidence/b5-component-validation-20260911.md) | PARTIAL |
| CD-003 | 182 code-design CD-003 | T006/T007 | plan sealer/parser; `Spec182PlanSealer/*` | canonical mismatch | `asan-ubsan`; independent decode | `DEV-76072468` | PO-003; [B5 component evidence](../evidence/b5-component-validation-20260911.md) | PARTIAL |
| CD-004 | 182 code-design CD-004 | T001/T006/T007 | grant request/verifier | signature/recipient/expiry | `tsan`; authority IO owner | `DEV-865e1ee2` | B1 evidence; PO-004 | PARTIAL |
| CD-005 | 182 code-design CD-005 | T006/T007 | native assembly worker; `Spec182NativeAssembly/*`, `DI_NativeOnnxAssemblyWorker` | path/key/frame/timeout | `asan-ubsan`; residue zero | `DEV-76072468` | PO-005; [B5 component evidence](../evidence/b5-component-validation-20260911.md) | PARTIAL |
| CD-006 | 182 code-design CD-006 | T006/T007 | tokenizer ABI/decoder; `Spec182NativeTokenizer/*`, `Spec182TokenizerFull/*` | malformed input/Python call | `parser-fuzz`; bounded decode | `DEV-76072468` | PO-006; [B5 component evidence](../evidence/b5-component-validation-20260911.md) | PARTIAL |
| CD-007 | 182 code-design CD-007 | T001/T002/T006/T007 | conversation/lifecycle owner | stale parent/cancel/close | `tsan`; lineage and terminal fencing | `DEV-865e1ee2` | B1/B2 evidence; PO-007/008 | PARTIAL |
| CD-008 | 182 code-design CD-008 | T006/T007 | Python binding facade | callback or ABI mismatch | `none`; thin forwarding | `DEV-865e1ee2` | B4 Python regression; PO-009 | PARTIAL |
| CD-009 | 182 code-design CD-009 | T006/T007 | library/build/install closure | stale library or missing target | `asan-ubsan`; source/artifact identity | `DEV-865e1ee2` | B3/B4 build records | PARTIAL |
| CD-010 | 182 code-design CD-010 | T005/T006/T007 | maintained caller migration | old route reconnected | `none`; explicit mode boundary | `DEV-865e1ee2` | caller matrix | PARTIAL |
| CD-011 | 182 code-design CD-011 | T006/T007 | qualification matrix/harness; `tests/python/test_spec182_native_closure.py` and `run-spec182-native-closure.py` | collector or secret failure | per row; complete child evidence | `DEV-76072468` | I/PO rows; [B5 component evidence](../evidence/b5-component-validation-20260911.md) | PARTIAL |
| CD-012 | 182 code-design CD-012 | T007/T008 | delivery/handoff | bundle identity mismatch | `none`; ordered manifest | `DEV-865e1ee2` | promotion candidate contract | OPEN |
| CD-013 | 182 code-design CD-013 | T006/T007 | input/artifact/provenance owner | wrong task/model/publication | `asan-ubsan`; binding and refusal | `DEV-865e1ee2` | B4 evidence; PO-013 | PARTIAL |
| CD-014 | 182 code-design CD-014 | T005/T006/T007 | shared Provider host | registration/stop/old runner | `tsan`; generation and bounded stop | `DEV-865e1ee2` | B4 evidence; PO-014 | PARTIAL |
| INV-001 | 182 spec INV-001 | T006/T007 | Core/DI boundary; `NativeInferenceProvider::serve` and scoped Core registration | DI policy enters Core | `none`; service-neutral Core | `DEV-76072468` | [B5 component evidence](../evidence/b5-component-validation-20260911.md); fresh convergence audit pending | PARTIAL |
| INV-002 | 182 spec INV-002 | T005/T006/T007 | C++ owner and Python facade | Python planner/state owner | `none`; no Python runtime owner | `DEV-865e1ee2` | caller matrix; PO-009/010 | PARTIAL |
| INV-003 | 182 spec INV-003 | T006/T007 | strategy/sealer/provider authority; `Spec182NativePlanning/*`, `Spec182PlanSealer/*`, `Spec182ProviderHost/*` | illegal proposal or grant | `asan-ubsan`; independent validation | `DEV-76072468` | PO-002/003/004; [B5 component evidence](../evidence/b5-component-validation-20260911.md) | PARTIAL |
| INV-004 | 182 spec INV-004 | T006/T007 | canonical wire and independent parser; `Spec182PlanSealer/*`, `Spec182NativeAssembly/*` | serializer drift | `asan-ubsan`; bytes agree | `DEV-76072468` | PO-003/005; [B5 component evidence](../evidence/b5-component-validation-20260911.md) | PARTIAL |
| INV-005 | 182 spec INV-005 | T001/T002/T006/T007 | shared normal/generation lifecycle | divergent cleanup | `tsan`; common owner and terminal cleanup | `DEV-865e1ee2` | B1/B2 evidence | PARTIAL |
| INV-006 | 182 spec INV-006 | T006/T007 | selection-after cold assembly | warm-only/preassembled shortcut | `asan-ubsan`; cold path and artifact ownership | `DEV-865e1ee2` | B4 provider selectors | PARTIAL |
| INV-007 | 182 spec INV-007 | T006/T007 | runtime process tree; canonical runner and C++ process owner | Python/libpython/helper appears | `asan-ubsan`; no interpreter and bounded exit | `DEV-76072468` | I02–I08 harness selectors; current process qualification pending | PARTIAL |
| INV-008 | 182 spec INV-008 | T006/T007/T008 | design/implementation/qualification separation | history or local PASS overclaimed | `none`; status/evidence agreement | `DEV-865e1ee2` | B1–B4 records | PARTIAL |
| INV-009 | 182 spec INV-009 | T007/T008 | Experimental vs external owner boundary | Tiger/SIF result mixed into local PASS | `none`; host ownership and transfer status | `DEV-865e1ee2` | transfer/promotion contracts | PARTIAL |

## Current YOLO Native Overlay (2026-09-11)

当前候选的 YOLO 运行证据覆盖本机可执行的 native C++ 路径：

| Case | Result | Evidence | Boundary |
| --- | --- | --- | --- |
| Y-A | `PASS_FOR_ROW` | `.codex-tmp/spec184-yolo-Y-A-output-20260911-r51/`；terminal response、C++ 数值 oracle、child exits 和 cleanup | 单 Provider `FullModel`，`onnxruntime-cpu`，`realCompute=true` |
| Y-B | `PASS_FOR_ROW` | `.codex-tmp/spec184-yolo-Y-B-output-20260911-r37/`；四 Provider terminal response、数值 oracle、runtime evidence 和 cleanup | `BackboneNeck`/两 Detect shard/`Merge` 多 Provider 路径 |
| Y-N | `PASS_FOR_ROW` | `.codex-tmp/spec184-yolo-Y-N-output-20260911-r50/`；`y-n-matrix-result.json` 与三份授权变异 evidence | `O/C/P/R/I/E/L` 七个边界；E 含 `EXPIRED`、`FORGED_AUTHORITY`、`WRONG_RECIPIENT` |

这些结果均绑定 `build-spec184-b5-candidate-r4/spec180-native-build.json`，并通过
`python3 scripts/spec180_native_build.py verify`；Y-A/Y-B 的数值 oracle 均为
`matched=true`、shape `[1,50,6]`、`maxAbsError=0.0005340576171875`。本机没有
`Qwen/Qwen3.6-27B` 的可执行资源，`Qwen3-0.6B` 只允许作为 smoke/ABI fixture，不能关闭
Qwen qualification row。

## Gate Rules

任何行缺少 selector/owner、candidate identity、child exit 或首失败边界时保持 `OPEN` 或
`PARTIAL`。每个有运行时状态的行还必须引用所属批次的 bounded `Dynamic Parameter Matrix`；
动态 profile 不能替代 C++ business oracle。T006 的矩阵静态合成和 fresh design-code
convergence audit 必须为 `PASS`，T007 才能开始；T007 的局部 selector PASS 不能提升其他行
或整个 Spec 的状态。当前 `DEV-865e1ee2` 标签只关联已验证的开发 checkpoint，不是可 promotion
的 candidate。
