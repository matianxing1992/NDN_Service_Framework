# Supersession and Evidence Transfer

**Decision**: 用户于2026-10-08要求停止Spec192/193，新Spec202承接全部目标/差距。**State**: SUPERSEDED_STOPPED；不是DONE，不删除或回滚旧结果。

## Intent Transfer

| Predecessor | Accepted intent / remaining | Spec202 owner |
| --- | --- | --- |
| Spec192 US1/T001,T002 | In-App/Persistent nativeowners、explicitarchive、目标/策略/readiness | US1/T001,T009,T010 |
| Spec192 US2/T003,T004 | handle/receipt/seedbarrier、异步进度/重启/幂等与durablecommit | US1/T001,T009,T010；sourcefailure新HIGH由T003修复 |
| Spec192 US3/T005 | history/currentavailability/fullref认证 | US3/T005 |
| Spec192 US4/T006 | nativefetch/Providerdedup/cooldown/globaldeadline | US2/T002,T006；postrename新HIGH追加 |
| Spec192 T007 | nativeofflinelegacyimport/源不变 | US4/T004；parentdurability新MEDIUM追加 |
| Spec192 T008,T009/US5 | thinPython/nativebinding与DIopaque复用 | US5/T008 |
| Spec192 T010 | design-code/currentfullclosure | US6/T009,T010；旧勾选不是当前新增缺陷修复证明 |
| Spec193 T001 | publicAPIinventory/独立oracle | T001/T007 |
| Spec193 T002 | A01–14、largerange及affectedtests | T001/T009/T010，G002修复T003；A06firstdiag只保留local历史 |
| Spec193 T003 | A15–22、A19/A21 gaps | T002/T005/T006/T010；纠正文档/oracle，不捏造API |
| Spec193 T004 | O01–17/L01–04、O17统一candidate | T005/T007/T010 |
| Spec193 T005/T006 | N01–10/P01–06 localclosure | T007/T008/T009/T010；不同candidate历史不合成formalqualification |
| Spec193 T007 | wholeaffected/currenttworounds/source | T009/T010 |
| Spec193 T008 | freshMiniNDNseed/readpersistence | T010跨run重验 |

## Predecessor Requirement Transfer

以下是需求继承的文档映射，不证明实现符合；T001逐入口核对，T009/T010分别关闭native/完整network资格。Spec192当前共有FR-001–025，不能遗漏后追加的可见性屏障。

| Spec192 requirement | Retained invariant | Contract / task / acceptance |
| --- | --- | --- |
| FR-001 | 双mode、独立配置、Persistent-only注册；Python launcher薄委托 | DB001/007；T001/T008/T009；A04/P01–06 |
| FR-002 | In-App耐久root/receipt，非session-only | DB001；T009/T010；A01–03 |
| FR-003 | In-App零network，转Persistent显式 | DB001；T009/T010；A01/A13 |
| FR-004 | 独立native Persistent、授权写、重启durability、显式placement | DB001/003/005；T003/T005/T009/T010；A06–12 |
| FR-005 | 完整immutable reference；handle不是授权或key | DB001/005/007；T005/T008/T009；A09/N/P |
| FR-006 | durable receipt、routeable区分、本地不计replica、ACK不是commit | DB001/003；T009/T010；A01/A06–08 |
| FR-007 | partial结构化、distinct authenticated Provider计数 | DB003；T003/T009/T010；A08/A14/R06 |
| FR-008 | 新进程复用reference；placement新version不改历史 | DB005；T005/T010；A15/A22及cross-run |
| FR-009 | receipt历史与当前availability分离；handle/seed解析不授权 | DB005；T005/T010；A16–20 |
| FR-010 | reachability/presence/integrity三维、timeout不是ABSENT | DB005；T005/T010；A17–20/O07 |
| FR-011 | principal去重、身份绑定、version-scoped cooldown | DB005/006；T006/T009/T010；A21 |
| FR-012 | monotonic overall deadline、原name/authorization/完整性、错误诊断 | DB002/005/006；T002/T006/T009/T010；A15/A20–21/R02；R01/R04故障恢复 |
| FR-013 | DI显式复用immutable物；跨app/host须archive | DB007；T008/T009/T010；P01–06/A13 |
| FR-014 | Repo domain-opaque；DI cache不计receipt/replica | DB007；T008/T009；P/DI conformance |
| FR-015 | 不自动存KV/activation；显式snapshot owner/confidentiality/retention | DB007；T001/T008/T009；DI入口/negative conformance |
| FR-016 | reference/result兼容；public Collaboration默认；显式Targeted，lower-level差异C++隔离 | DB001/007；T001/T007/T008/T009；默认/显式mode及compat caller conformance |
| FR-017 | 完整suite/MiniNDN之前CodeGraph convergence；gap保持BLOCK | candidate合同；T009/T010；fresh五lane审查 |
| FR-018 | discovery ACK零storage；caller selected set/policy后写 | DB001/003；T009/T010；A05/A07/A09 |
| FR-019 | owner签名LOCATION、KeyLocator/Response identity、fullref/currentreceipt/version/expiry；hint不授权 | DB005；T005/T009/T010；A17/A22/N/O17 |
| FR-020 | DI/Repo reference显式versioned binding、digest/size交叉校验、cache不绕native授权 | DB007；T008/T009/T010；P/DI conformance |
| FR-021 | selected seed journal/commit屏障、native worker、submitter status auth、无caller token持久/转发 | DB001/003/005；T003/T005/T009/T010；A10–12/A14 |
| FR-022 | 双client不同owner/handle/receipt；explicit archive保持reference、consumer不依赖app root | DB001；T009/T010；A13/A15 |
| FR-023 | 全Repo行为C++唯一owner；Python production绑定、完整C++调用无Python | DB007；T007/T008/T009；N/P与native suites |
| FR-024 | 显式offline C++ import、WAL snapshot、allowlist/source不变、inventory/provenance/失败关闭 | DB004/007；T004/T007/T009/T010；P05/R07及native import selectors |
| FR-025 | operation-owned artifact在exact durable receipt前所有local/network入口不可见；legacy/import例外须proof | DB001/007；T007/T009/T010；O/local list/FETCH/RANGE/MANIFEST/INVENTORY barrier conformance |

| Spec193 requirement | Retained invariant | Contract / task / acceptance |
| --- | --- | --- |
| FR-001 | 每公开C++/binding/DI入口双向清单；local在多节点run内清楚标注 | DB001/007；T001/T007/T008/T010；逐入口inventory |
| FR-002 | 至少五独立NFD host；三replica使用三principal | candidate合同；T010；five/six/seven-host manifests |
| FR-003 | 成功/拒绝/timeout/partial/restart/loss/replay/auth/cross-language稳定case | matrix；T009/T010；全部59case/subscenario |
| FR-004 | 首失败/最小复现/根因/修复/affected与samecandidate复验 | DB002–004/candidate；T002–T010；R01–07/failure-log |
| FR-005 | Spec192原candidate/evidence不变；新独立hash，不扩大旧PASS | 本文/candidate；T001/T009/T010；pre-stop快照、锁与history |
| FR-006 | static/unit/integration在formalnetwork之前；pcap/exit/boundedcleanup | candidate合同；T009/T010；fresh gates和每run manifest |
| FR-007 | original Data完整wire/name/producer signature、durable DATA_RECEIPT/认证DATA_LOCATION、每Interest指定hint；敏感bytes生产者加密 | DB005；T005/T009/T010；O08/O09/O17全部subscenario；rawInterest本身不携带NDNSF grant |

## Preserved Evidence

所有旧contract/checklists/checkbox及原始run目录保留。六个旧spec/plan/tasks原始字节已先保存至`evidence/predecessor-documents-before-stop.zip`（SHA`9abfd3d57946ab87552c52e3dde7c4c9dbdb94c49d229ea40dc195706b7e7cb8`）；stopnotice只追加到顶部，旧spec状态改为SUPERSEDED_STOPPED并更新活动指针，不重写历史事实、不将未完成标DONE、不继续旧taskledger。新202matrix有独立NOT_RUN状态，通过链接继承旧case定义与history；旧hash-bounddoc输入需要原版本时从此快照恢复，不把加过stopnotice的文档冒充原候选字节。

末次A06_LARGE `a06-large-diag-001` PASS/minindn_cleanupPASS，candidate278aca45…；process已终止，无活动MiniNDN/build，本轮无第二轮。其131123B成功不能取消G001/G002或替代完整qualification。旧冻结证据不存在任何新的“全部通过”声明。

## Operating Authority

2026-10-08后续用户决定：本机Repo工作由`Experimental`转到保留分支`Repo`。新分支从`e6ff3409`建立；switch前后HEAD/status/diff/staged及本地规划文档hash相同，原未提交实现、fixture与证据留在同一工作树，未自动提交源码。`Experimental`保留原checkpoint，旧历史candidate不会因分支名变化被重新标PASS。Spec202的当前branch字段为Repo；本审计原baseline仍是0957f675+当时overlay。

`.specify/feature.json`与AGENTS managedplan指向202；ContextMode按canonicalsharedguide重新index/health，CodeGraph仍核对实际source。spec192/193名称的generic source/test入口可由202维护，不能机械复制或另建Tiger-only目录。新产品实施只在明确实施202授权后启动，本轮完成静态规划即可。
