# Spec184 Qualification Matrix

**Status**: PARTIAL / T006 row binding complete; candidate and qualification remain open
**Source**: [transfer matrix](transfer-matrix.md), inherited Spec182 proof contracts, and current checkpoint `6e722998`

本表是最终资格的唯一行级入口。迁移记录的结构检查不能关闭任何一行；每行必须绑定
当前 candidate、源码/二进制身份、真实 C++ target/selector 或外部 owner、负例、所有子
进程 exit、cleanup 和 evidence path。`TRANSFERRED` 只表示归属，`PASS` 只允许来自当前
candidate 的完整结果。

## Inherited Parent Obligations

| Row | Inherited obligation | Spec184 owner | Required evidence | Dynamic profile / invariant | Initial status |
| --- | --- | --- | --- | --- | --- |
| 182:T004 | canonical seal/非法投影与 Core/Provider 真实接收资格 | T006/T007 | C++ selector、wire/source identity、负例结果 | `asan-ubsan`; canonical bytes and rejection cleanup | OPEN |
| 182:T005 | grant IO 缺陷 F-01、签发/验证/消费完整资格 | T001/T006/T007 | `Spec184AuthorityIoOwnership`、真实 authority/provider 结果 | `tsan`; IO owner and pending-call balance | OPEN |
| 182:T006 | cold ONNX 装配 bytes/取消/清理、Python helper 退出验收 | T006/T007 | native assembly target、process exit 和 cleanup | `asan-ubsan`; runner lifetime and residue=0 | OPEN |
| 182:T007 | tokenizer encode/decode oracle 与无解释器依赖 | T006/T007 | C++ tokenizer selector、dependency/source closure | `parser-fuzz`; malformed wire bounded failure | OPEN |
| 182:T008 | 原生输入/模型/工件准备与 admission 完整验收 | T006/T007 | preparation/admission C++ target、artifact-bound result | `asan-ubsan`; artifact ownership and cleanup | OPEN |
| 182:T009 | 共用 Provider host 注册、执行、停止与入口一致性 | T005/T006/T007 | provider target、caller row、stop/cleanup evidence | `tsan`; stop/join ordering where async | OPEN |
| 182:T010 | F-01/F-02、完整 request lifecycle/负例 | T001/T002/T006/T007 | B1 selectors、交错/取消/晚回调结果 | `tsan`; request state and callback fencing | OPEN |
| 182:T011 | F-03/F-04、continuation/受限恢复/lineage 剩余资格 | T003/T004/T006/T007 | durable/checkpoint selectors、独立读取与失败保留 | `asan-ubsan`; journal/checkpoint lifetime | OPEN |
| 182:T012 | 薄绑定支持模式，无 Python strategy/state owner | T005/T006/T007 | binding/source closure、native owner and parity evidence | `none` for static closure; runtime row profile as applicable | OPEN |
| 182:T013 | 维护默认路由与 legacy retirement | T005/T006/T007 | caller matrix、default route、zero-use/rollback evidence | `tsan` for async callers; otherwise `none` | OPEN |
| 182:T014 | no-Python harness/collector、依赖排除和负例 | T006/T007 | C++ process/no-Python owner、trace pairing and exits | `asan-ubsan`; child cleanup and bounded exit | OPEN |
| 182:T015 | FR/CD/INV/PO、接线、oracle、build、设计总对账 | T006 | row completeness、fresh convergence audit | `none`; documentation/matrix invariants only | OPEN |
| 182:T016 | 同源完整 unit/integration、MiniNDN/no-Python 本地资格 | T007 | candidate-bound complete results and child exits | per row profile; terminal cleanup and identity binding | OPEN |
| 182:T017 | 唯一交付基线、维护文档、两入口示例与外部边界 | T008 | final source/bundle identity and handoff | `none`; evidence/status agreement | OPEN |

## Required Additional Rows

T006 必须在本表追加一行对应每个 `PO-001`–`PO-016`，以及适用的 `I`, `FR`, `CD`,
`INV`。不得使用 `PO-*` 或 `I-*` 通配行代替实际行。新增行至少包含：

`rowId`, `sourceContract`, `ownerTask`, `productionEntry`, `C++ target/selector or external owner`,
`negative/recovery boundary`, `riskClass`, `dynamicProfile`, `dynamicInvariant`, `candidateId`,
`source/runtime/config hashes`, `evidencePath`, `status`。

## PO Closure Rows

以下是当前逐项盘点。`DEV-865e1ee2` 只是本地开发 checkpoint 的关联标签，尚未成为
promotion candidate；`PARTIAL`/`OPEN` 不得被 T007 当作资格通过。

| rowId | sourceContract | ownerTask | productionEntry / C++ target or external owner | negative/recovery boundary | riskClass / dynamicProfile / dynamicInvariant | candidateId / source-runtime-config identity | evidencePath | status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| PO-001 | 182 proof-design PO-001 / FR-001/012 / CD-001/009 | T006/T007 | installed C++ consumer → native requester/provider; T016 owner for process run | interpreter or DI Python dependency appears; child/trace incomplete | isolation / `asan-ubsan` / no Python, bounded child cleanup | `DEV-865e1ee2`; source checkpoint, runtime hash per run, config `UNFROZEN` | 182 R11-B9-G7 current stream evidence; B5 rerun required | PARTIAL |
| PO-002 | 182 proof-design PO-002 / FR-003/009 / CD-002 | T006/T007 | `NativeModelSplitStrategy` + `NativePlacementStrategy`; existing C++ unit target | invalid rank/device/lease/cut vector | planning / `none` or `asan-ubsan` / legal cut and deterministic order | `DEV-865e1ee2`; source `865e1ee2`, runtime/config `UNFROZEN` | inherited proof-design; exact current selector not bound | OPEN |
| PO-003 | 182 proof-design PO-003 / FR-002/004 / CD-003 | T006/T007 | `NativePlanSealer` → Core `CommitCollaborationPlan` | missing endpoint, ACK digest mismatch, duplicate commit | serialization / `asan-ubsan` / canonical bytes and independent parser agreement | `DEV-865e1ee2`; source/runtime/config `UNFROZEN` | inherited proof-design; current cross-layer evidence pending | OPEN |
| PO-004 | 182 proof-design PO-004 / FR-005 / CD-004 | T001/T006/T007 | C++ requester → independent authority → verifier | expired, wrong recipient, forged signature, key bypass | authorization / `tsan` / IO owner and grant binding | `DEV-865e1ee2`; B1 runtime hashes, authority config not frozen | B1 evidence; independent process qualification pending | PARTIAL |
| PO-005 | 182 proof-design PO-005 / FR-006/016 / CD-005 | T006/T007 | `NativeProviderHost` selection → assembler | recipe/node/path/ciphertext/key mismatch; worker timeout | assembly/lifetime / `asan-ubsan` / certified artifact ownership and residue zero | `DEV-865e1ee2`; provider digest `ff00c53e...`, model/config `UNFROZEN` | B4 post-selection and assembly selectors; cold process case pending | PARTIAL |
| PO-006 | 182 proof-design PO-006 / FR-007 / CD-006 | T006/T007 | native tokenizer adapter and C++ decoder | digest mismatch, Unicode/special/byte-fallback error, Python helper | parser/lifetime / `parser-fuzz` / exact text and bounded malformed-input failure | `DEV-865e1ee2`; tokenizer artifact/config `UNFROZEN` | inherited tokenizer design; current C++ selector not bound | OPEN |
| PO-007 | 182 proof-design PO-007 / FR-008 / CD-001/007 | T001/T002/T006/T007 | `NativeInferenceClient` request/stream lifecycle | cancel/deadline/late callback/close/revival | concurrency / `tsan` / one terminal state and no stale callback | `DEV-865e1ee2`; B1 TSan and B2 runtime hashes | B1/B2 evidence; process cancellation rows pending | PARTIAL |
| PO-008 | 182 proof-design PO-008 / FR-008/016 / CD-007 | T006/T007 | native Qwen conversation/continuation owner | wrong parent, duplicate prefix, replacement fencing | continuation / `asan-ubsan` / lineage and prefix monotonicity | `DEV-865e1ee2`; Qwen artifact/config `UNFROZEN` | B4 Qwen focused selectors; complete token oracle pending | PARTIAL |
| PO-009 | 182 proof-design PO-009 / FR-010 / CD-008 | T006/T007 | C++ consumer and optional Python facade | Python strategy callback or input mismatch | binding / `none` / native and facade semantics agree without callback execution | `DEV-865e1ee2`; extension/runtime/config `UNFROZEN` | B4 38-test route regression; full parity matrix pending | PARTIAL |
| PO-010 | 182 proof-design PO-010 / FR-011/016 / CD-010 | T005/T006/T007 | maintained caller rows and legacy exclusion checks | old provider/coordinator/helper reconnected | routing / `none` / explicit native default and zero-use observation | `DEV-865e1ee2`; caller matrix source `865e1ee2`, runtime/config `UNFROZEN` | caller matrix and B4 evidence | PARTIAL |
| PO-011 | 182 proof-design PO-011 / FR-013/014 / CD-011 | T006/T007 | C++ unit/integration/process plus MiniNDN owner | startup/collector/secret/exit failure misreported as protocol result | qualification / inherited row profiles / candidate identity and terminal cleanup | `DEV-865e1ee2`; no promoted candidate | 182 case manifest; full current matrix not run | OPEN |
| PO-012 | 182 proof-design PO-012 / FR-014/015 / CD-012 | T006/T008 | clean checkout and handoff receiver | source/dependency/model/config/harness mismatch | delivery / `none` / ordered manifest identity and preflight refusal | `DEV-865e1ee2`; source checkpoint only, bundle/config `UNFROZEN` | promotion candidate contract | OPEN |
| PO-013 | 182 proof-design PO-013 / FR-001/002/004/009/016 / CD-013 | T006/T007 | native `prepareInput`/`inspectModel`/`ensureArtifacts`/`verify` | wrong provenance, catalog/publication name or candidate | provenance / `asan-ubsan` / request/artifact/model identity binding | `DEV-865e1ee2`; source `865e1ee2`, artifacts/config `UNFROZEN` | B4 route/assembly selectors; full preparation matrix pending | PARTIAL |
| PO-014 | 182 proof-design PO-014 / FR-001/009/010/012 / CD-014 | T005/T006/T007 | native consumer/CLI/binding → shared Provider host | old Python runner, early handler destruction, unrelated service stop | host/lifetime / `tsan` / registration generation and bounded stop | `DEV-865e1ee2`; provider binary `ff00c53e...`, runtime/config `UNFROZEN` | B4 provider selectors; real shared-host process row pending | PARTIAL |
| PO-015 | 182 proof-design PO-015 / FR-018 / CD-001--014 | T006 | review-agent plus Spec184 convergence audit | missing caller, test registration, source closure or design contradiction | review / `none` / five lanes complete and findings dispositioned | `DEV-865e1ee2`; contract hash changes after B4 require refresh | B1–B4 review traces; fresh B5 convergence not written | PARTIAL |
| PO-016 | 182 proof-design PO-016 / FR-019 / CD-001--014 | T007/T008 | T007 complete run and T008 handoff | any required row unrun or unbound | qualification / per row / all required local results share candidate identity | `DEV-865e1ee2`; no promoted candidate | no final qualification record | OPEN |

## Isolation Rows

These rows preserve the eight inherited no-Python/collector counterfactuals. They are external
owner cases, not substitutes for native C++ behavior selectors; Python only operates the harness.

| rowId | sourceContract | ownerTask | productionEntry / C++ target or external owner | negative/recovery boundary | riskClass / dynamicProfile / dynamicInvariant | candidateId / source-runtime-config identity | evidencePath | status |
| --- | --- | --- | --- | --- | --- | --- | --- |
| I01 | 182 native-isolation-design I01 | T006/T007 | canonical installed native consumer; MiniNDN owner | valid native consumer must complete with full evidence | isolation / `none` / marker, namespace, trace and cleanup complete | `DEV-865e1ee2`; installed consumer identity pending | 182 R11-B9-G7 / I01 evidence | PARTIAL |
| I02 | 182 native-isolation-design I02 | T006/T007 | canonical runner with fork-helper and renamed Python ELF counterfactual | exec/source identity violation rejected before business result | isolation / `asan-ubsan` / rejection boundary and no fallback | `DEV-865e1ee2`; runner/model/config `UNFROZEN` | `test_helper_exec_rejected` planned selector | OPEN |
| I03 | 182 native-isolation-design I03 | T006/T007 | runtime mapping detector | transient or renamed `libpython` mapping fails | isolation / `asan-ubsan` / complete mapping observation | `DEV-865e1ee2`; runtime closure `UNFROZEN` | `test_transient_python_mapping_rejected` planned selector | OPEN |
| I04 | 182 native-isolation-design I04 | T006/T007 | endpoint detector plus native requester/provider | host TCP/abstract UNIX/undeclared socket rejected while NFD allowlist works | isolation / `tsan` / declared endpoint-only communication | `DEV-865e1ee2`; topology/config `UNFROZEN` | `test_undeclared_endpoint_rejected` planned selector | OPEN |
| I05 | 182 native-isolation-design I05 | T006/T007 | trace and child collector | missing trace tail/short-lived child/observer kill is `UNQUALIFIED` | evidence / `none` / observation completeness and bounded result | `DEV-865e1ee2`; harness digest `UNFROZEN` | `test_incomplete_observation_unqualified` planned selector | OPEN |
| I06 | 182 native-isolation-design I06 | T006/T007 | cold native requester/provider process | warm-only model, harness-generated plan/text, or missing Provider fails | assembly / `asan-ubsan` / cold preparation, role coverage and independent oracle | `DEV-865e1ee2`; model/config `UNFROZEN` | `test_cold_path_and_role_coverage_required` planned selector | OPEN |
| I07 | 182 native-isolation-design I07 | T006/T007 | external Python harness with native business processes | harness Python is allowed; business owner must remain no-Python | isolation / `none` / process/library allowlist and owner evidence | `DEV-865e1ee2`; process manifest `UNFROZEN` | `test_external_harness_excluded` planned selector | OPEN |
| I08 | 182 native-isolation-design I08 | T006/T007 | process-group and descendant collector | detached/late child leaves no owned process or becomes `UNQUALIFIED` | lifetime / `tsan` / PID lineage and cleanup completeness | `DEV-865e1ee2`; runner config `UNFROZEN` | `test_descendant_cleanup_required` planned selector | OPEN |

## Functional and Design Crosswalk

每个继承 ID 单独列出，避免用范围或通配符掩盖缺口。`sourceContract` 指向 Spec182 原
契约；`entry/selector` 可在 T006/T007 继续替换为当前同源 C++ 选择器。

| rowId | sourceContract | ownerTask | entry / selector | boundary | profile / invariant | candidateId | evidencePath | status |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| FR-001 | 182 spec FR-001 | T001/T006/T007 | NativeInferenceClient + native consumer | missing authority/preparation or wrong source | `tsan`; owner and provenance binding | `DEV-865e1ee2` | B1 evidence; PO-004/013 | PARTIAL |
| FR-002 | 182 spec FR-002 | T006/T007 | NativePlanSealer | missing/changed canonical field | `asan-ubsan`; canonical bytes | `DEV-865e1ee2` | PO-003 | OPEN |
| FR-003 | 182 spec FR-003 | T006/T007 | NativeModelSplitStrategy | illegal graph cut or budget | `none`; deterministic legal proposal | `DEV-865e1ee2` | PO-002 | OPEN |
| FR-004 | 182 spec FR-004 | T006/T007 | Core plan commit/parser | duplicate or tampered plan | `asan-ubsan`; independent parser agreement | `DEV-865e1ee2` | PO-003 | OPEN |
| FR-005 | 182 spec FR-005 | T001/T006/T007 | NativeGrantClient/Verifier | wrong signer, recipient or expiry | `tsan`; grant binding and pending balance | `DEV-865e1ee2` | B1 evidence; PO-004 | PARTIAL |
| FR-006 | 182 spec FR-006 | T006/T007 | NativeCanonicalOnnxAssembler | recipe/path/key mismatch | `asan-ubsan`; artifact ownership | `DEV-865e1ee2` | B4 evidence; PO-005 | PARTIAL |
| FR-007 | 182 spec FR-007 | T006/T007 | NativeTokenizer/decoder | malformed ids or Python helper | `parser-fuzz`; exact text and bounded error | `DEV-865e1ee2` | PO-006 | OPEN |
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
| FR-019 | 182 spec FR-019 | T006/T007 | native-first verification order | later phase hides earlier gap | per row; phase exit is explicit | `DEV-865e1ee2` | plan/tasks | OPEN |
| CD-001 | 182 code-design CD-001 | T006/T007 | NativeInferenceClient public API | invalid input/options/close | `tsan`; ownership and error mapping | `DEV-865e1ee2` | B1 evidence; PO-007 | PARTIAL |
| CD-002 | 182 code-design CD-002 | T006/T007 | split/placement strategies | illegal proposal | `none`; deterministic legal output | `DEV-865e1ee2` | PO-002 | OPEN |
| CD-003 | 182 code-design CD-003 | T006/T007 | plan sealer/parser | canonical mismatch | `asan-ubsan`; independent decode | `DEV-865e1ee2` | PO-003 | OPEN |
| CD-004 | 182 code-design CD-004 | T001/T006/T007 | grant request/verifier | signature/recipient/expiry | `tsan`; authority IO owner | `DEV-865e1ee2` | B1 evidence; PO-004 | PARTIAL |
| CD-005 | 182 code-design CD-005 | T006/T007 | native assembly worker | path/key/frame/timeout | `asan-ubsan`; residue zero | `DEV-865e1ee2` | B4 evidence; PO-005 | PARTIAL |
| CD-006 | 182 code-design CD-006 | T006/T007 | tokenizer ABI/decoder | malformed input/Python call | `parser-fuzz`; bounded decode | `DEV-865e1ee2` | PO-006 | OPEN |
| CD-007 | 182 code-design CD-007 | T001/T002/T006/T007 | conversation/lifecycle owner | stale parent/cancel/close | `tsan`; lineage and terminal fencing | `DEV-865e1ee2` | B1/B2 evidence; PO-007/008 | PARTIAL |
| CD-008 | 182 code-design CD-008 | T006/T007 | Python binding facade | callback or ABI mismatch | `none`; thin forwarding | `DEV-865e1ee2` | B4 Python regression; PO-009 | PARTIAL |
| CD-009 | 182 code-design CD-009 | T006/T007 | library/build/install closure | stale library or missing target | `asan-ubsan`; source/artifact identity | `DEV-865e1ee2` | B3/B4 build records | PARTIAL |
| CD-010 | 182 code-design CD-010 | T005/T006/T007 | maintained caller migration | old route reconnected | `none`; explicit mode boundary | `DEV-865e1ee2` | caller matrix | PARTIAL |
| CD-011 | 182 code-design CD-011 | T006/T007 | qualification matrix/harness | collector or secret failure | per row; complete child evidence | `DEV-865e1ee2` | I/PO rows | OPEN |
| CD-012 | 182 code-design CD-012 | T007/T008 | delivery/handoff | bundle identity mismatch | `none`; ordered manifest | `DEV-865e1ee2` | promotion candidate contract | OPEN |
| CD-013 | 182 code-design CD-013 | T006/T007 | input/artifact/provenance owner | wrong task/model/publication | `asan-ubsan`; binding and refusal | `DEV-865e1ee2` | B4 evidence; PO-013 | PARTIAL |
| CD-014 | 182 code-design CD-014 | T005/T006/T007 | shared Provider host | registration/stop/old runner | `tsan`; generation and bounded stop | `DEV-865e1ee2` | B4 evidence; PO-014 | PARTIAL |
| INV-001 | 182 spec INV-001 | T006/T007 | Core/DI boundary | DI policy enters Core | `none`; service-neutral Core | `DEV-865e1ee2` | convergence audit pending | OPEN |
| INV-002 | 182 spec INV-002 | T005/T006/T007 | C++ owner and Python facade | Python planner/state owner | `none`; no Python runtime owner | `DEV-865e1ee2` | caller matrix; PO-009/010 | PARTIAL |
| INV-003 | 182 spec INV-003 | T006/T007 | strategy/sealer/provider authority | illegal proposal or grant | `asan-ubsan`; independent validation | `DEV-865e1ee2` | PO-002/003/004 | OPEN |
| INV-004 | 182 spec INV-004 | T006/T007 | canonical wire and independent parser | serializer drift | `asan-ubsan`; bytes agree | `DEV-865e1ee2` | PO-003/005 | OPEN |
| INV-005 | 182 spec INV-005 | T001/T002/T006/T007 | shared normal/generation lifecycle | divergent cleanup | `tsan`; common owner and terminal cleanup | `DEV-865e1ee2` | B1/B2 evidence | PARTIAL |
| INV-006 | 182 spec INV-006 | T006/T007 | selection-after cold assembly | warm-only/preassembled shortcut | `asan-ubsan`; cold path and artifact ownership | `DEV-865e1ee2` | B4 provider selectors | PARTIAL |
| INV-007 | 182 spec INV-007 | T006/T007 | runtime process tree | Python/libpython/helper appears | `asan-ubsan`; no interpreter and bounded exit | `DEV-865e1ee2` | I rows; no current qualification | OPEN |
| INV-008 | 182 spec INV-008 | T006/T007/T008 | design/implementation/qualification separation | history or local PASS overclaimed | `none`; status/evidence agreement | `DEV-865e1ee2` | B1–B4 records | PARTIAL |
| INV-009 | 182 spec INV-009 | T007/T008 | Experimental vs external owner boundary | Tiger/SIF result mixed into local PASS | `none`; host ownership and transfer status | `DEV-865e1ee2` | transfer/promotion contracts | PARTIAL |

## Gate Rules

任何行缺少 selector/owner、candidate identity、child exit 或首失败边界时保持 `OPEN` 或
`PARTIAL`。每个有运行时状态的行还必须引用所属批次的 bounded `Dynamic Parameter Matrix`；
动态 profile 不能替代 C++ business oracle。T006 的矩阵静态合成和 fresh design-code
convergence audit 必须为 `PASS`，T007 才能开始；T007 的局部 selector PASS 不能提升其他行
或整个 Spec 的状态。当前 `DEV-865e1ee2` 标签只关联已验证的开发 checkpoint，不是可 promotion
的 candidate。
