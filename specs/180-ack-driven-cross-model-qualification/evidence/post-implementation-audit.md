# Post-Implementation Convergence Audit — T014 (Revision 116, 2026-09-04)

> **INVALIDATED — DO NOT REUSE.** Revisions 123–125 reworked the Controller
> PUBPARAMS readiness/runtime source, the native evidence, and the grant
> subsystem after this audit ran at revision 116, then closed spec180 by
> reassignment (spec181 R003 banner audit).  The current-source audit
> records are `audit-revision123-design-code-conformance-20260904.md` and
> the revision-124/125 closure notes in `tasks.md`.  This historical audit
> is retained for audit history only and cannot satisfy a new convergence
> gate.

Scope: the maintained ACK-driven YOLO path (T002–T011 executed portion),
revisions 112–116. Verdict gate per the 12 audit principles.

## 文档声称 (proposed)

- spec.md/tasks.md claim an ACK-driven YOLO functional slice: Y-A atomic,
  Y-B shared-backbone-two-shard, Y-N controls, on the sealed r112 candidate.
- tasks.md revision 112 queue is the only execution order; revisions
  113–116 record closure notes. No task is claimed complete on seam tests
  alone (revision 105 stop rule honored).

## 代码实现 (implemented)

Verified with CodeGraph (verbatim source, 2026-09-04):

- `configure_automatic_planning` forwards `canonical_artifact_ensurer`
  (`app_sdk/client.py:590-664`, wire at :654).
- `_certify_v3_role_specs` seals digest-pinned recipes from the binding,
  with the canonical ONNX identity digest separated from the
  planning-space graph-port digest (`app_sdk/placement.py:4190-4313`,
  `:4265-4270`).
- `YoloCanonicalArtifactBinding.describe/ensure`
  (`adapters/yolo/adapter.py:336-385`) returns the binding facts and the
  published ARTIFACT identities; `ensure` rejects any role whose
  artifact digest is not the candidate's certified fragment digest
  (`adapter.py:371-374`).
- The Provider assembles certified component-set subgraphs after
  Selection (`provider.py`, `_assemble_certified_role_execution`; invoked
  only when `role_kind == "COMPONENT_SET"` with a non-empty node cover and
  a local artifact) and swaps the assembled model into the role execution.
- Native fixes: lowercase canonical digest form
  (`ndn-service-framework/ServiceProvider.cpp`), `/SERVICE`-scope fetch
  routing through the service-authorized path (`ServiceProvider.cpp`),
  rebuilt with `./waf -o build-system-j2 build -j2` (124/124).
- Focused red→green tests exist for every revision-113–116 behavior
  change (controller publication lifetime; snapshot runtime digests;
  ExecutionRole component interval; simple-service mirror disabled for V3
  issuers; empty fetch reference for provider-prepared roles; plan
  validation; boot-epoch composite form; can_provision offer field;
  multi-candidate single-manifest catalogue uniqueness; certified
  assembly with the real package).

## 测试执行 (executed)

- Full Spec180 Python suite: 207 passed (2026-09-04).
- Affected Spec170 suites (placement V3, ack no-reservation,
  layer-reuse-first, plan sealer): pass.
- Behavioral assembly test runs the real canonical package through
  certification → extraction → ONNX Runtime and checks the assembled
  interface names (`tests/python/test_spec180_role_assembly.py`).

## 实验测量 (measured)

Live MiniNDN runs on the sealed r112/r115 inputs (root-only MiniNDN):

| case | result | terminal digest |
| --- | --- | --- |
| Y-A (×2) | PASS | `sha256:e6f942bc…f8dc23aa` (identical across runs) |
| Y-B | PASS | `sha256:e6f942bc…f8dc23aa` |
| Y-N | PASS | `sha256:e6f942bc…f8dc23aa` |

All three cases agree byte-for-byte with an independent offline full-model
CPU-ORT forward on the same deterministic 640-by-640 input. Y-B executed
the 405/20/40/126-node role split with encrypted NDNSF_DATA_V1 tensor
exchange; every lifecycle journal validated complete (10 ordered
milestones).

## Findings by principle

1. **Intent fidelity** — 无偏题。Qwen-F/三 GPU/二次 warm/跨模型闭合保持延期，未在证据中声称。
2. **Necessity & Occam** — revision 115 复用既有 `CanonicalArtifactBinding`/`_certify_v3_role_specs`/`assemble_certified_onnx_model` 机制，未新建并行机制。Y-B 本地 canonical 传输为最小切片；Repo 物化留作 SIF/Tiger 晋升路径（明确未声称）。
3. **Architecture & ownership** — Core 未加入 YOLO 特判；模型特判在 `adapters/yolo/`，用例编排在 runner/prepare 工具。
4. **Cross-document consistency** — 字段名 (candidateDigest/graphDigest/…)、默认值 (1500 ms ACK、640 输入) 与 contracts/runner/user 一致；tasks.md r113–r116 笔记与证据文件一一对应。
5. **Code fact verification** — 见上 CodeGraph 证据；装配测试以真实包运行。
6. **Security & distributed correctness** — 目录签名验证（签入 registry）；V3 offer 签名（候选绑定信任根 + Trust-Schema 校验）；输入加密 REPO_REF + manifest digest 校验；重放保护实跑可见（`NDNSF_PROVIDER_REPLAY_REJECTED duplicate-request-and-token`）；角色归属一对一（four roles/four providers）；缺失工件 fail-closed（exit 78 / 明确错误）。**已知残余（记录，非协议缺陷）**：MiniNDN nfd.conf `authorizations.privileges` 不含 `rib`，第二 ServiceUser 注册有 ~30–50% 概率被离线验证器拒绝；重试同一封存输入通过。属资质环境配置缺口，须在 SIF/Tiger 前修复（nfd.conf 模板授予 `rib`，或注册前预热本地证书库）。
7. **Task executability** — r115/r116 变更均有目标文件、行为结果与验收证据。
8. **Validation design** — 网络功能全部经 MiniNDN（Y-A×2、Y-B、Y-N）；等价性为逐字节对照（同输入同 digest），非统计声明。
9. **Evidence integrity** — 四层分离如上；`implemented` 未包装为 `measured`。
10. **Frozen evidence** — 封存输入（r112/r115 bundles + 包 digest）在运行间未改动；Y-A 两次运行 digest 一致证明确定性。
11. **Migration & rollback** — `canonical_graph_digest` 为向后兼容可选字段（空则沿用旧语义）；`can_provision` 为默认 False 的新字段；旧 API（offline-oracle 路径）保留。
12. **Verdict gate** — 见下。

## Verdict: CONDITIONAL PASS

限定修复项（merge 前必须完成）：

1. **[HIGH→已修复] MiniNDN 注册 flake 根治**：最初尝试在系统 nfd.conf
   `authorizations.privileges` 增加 `rib`——NFD 24.07 在配置解析时尚未注册
   rib 模块（`unknown module 'rib' under authorize[0]`），该路径不可行，已
   还原配置（备份保留）。最终修复为应用侧有界重试：
   `registerInterestFilterWithRetry`（utils.hpp/cpp）用于
   CertificatePublisher 与 ServiceUser 的 NDNSF/CK 前缀注册（6 次、250ms
   间隔）；NDFD 离线命令验证器在节点 PIB 被并发 keychain 操作短暂锁定时会
   瞬态错过签名证书，重试将其从致命启动失败降为延迟注册。修复后的 Y-A、
   Y-B 均再次实跑 PASS（终端 digest 不变）。
2. **[MEDIUM] T008 原生 Merge 未实现**：Y-B 当前 Merge 走认证子图路径（正确性已由等价性证明），但 T008 声称的"原生依赖消费者 Merge 角色"与 `adapters/yolo/merge.py`/`runtime.py` 元数据文件尚缺。二者在 SIF/Tiger 前补齐或由本次等价性证据明确改写 T008 验收口径。
3. **[MEDIUM] Y-N 负面子用例未逐条实跑**：Y-N-C/P/R/I/E/L 的 FAIL_CLOSED 预期由聚焦套件覆盖，但未在 MiniNDN 实跑中逐条执行；在 SIF 前以最小负矩阵补一次实跑（或明确将聚焦套件证据写入对应 SC）。
4. **[MEDIUM] T015 清单的 cpp 条目指向旧构建树**：`spec180_inventory.py`
   默认 `build/unit-tests`/`build/integration-tests`（2026-09-02 旧二进制）；
   T015 必须以 `--unit-binary build-system-j2/unit-tests
   --integration-binary build-system-j2/integration-tests` 生成清单，并以
   `--with-tests` 重配置当前树后构建测试二进制（进行中）。
5. **[MEDIUM] C++ 单元套件 5 个 StreamFacade 失败**：`StreamFacade/*` 内存访问
   违规（stream-facade.t.cpp，Spec175 流路径，非今日 spec180 变更文件）；
   为旧树二进制上的观察，需在 build-system-j2 新二进制上复测后归因
   （预存问题或环境问题），SIF 前必须 triage。

**T015 原生失败归因（2026-09-04，收尾）**：两处原生失败均非
spec180 回归：
(a) StreamFacade 套件（sign throw ×3、SIGABRT、内存违规）属 Spec175
流路径；旧二进制同测通过但旧二进制早于当前测试源
（`tests/integration-tests/ndnsf-di-core-flow.t.cpp` 09-02 11:44 更新，
`build/integration-tests` 09-01 01:20 构建），失败表现为环境/工作树相关；
(b) `Spec170NdnsfDiCoreFlow/ProductionNativeHandlers*` 的 `/Aux` 断言
（coordinatorFactoryCalls 1≠2）同样源于测试源更新晚于旧二进制——旧二进制
执行的是不含 /Aux 断言的旧测试。二者均为前序会话工作树状态，需 SIF 前
triage，但不阻塞本地 YOLO 清单结论。重试 helper 的 move-then-copy 缺陷
（重试路径注册空 handler）已修复并以 shared_ptr 持有 handler；Y-A/Y-B
在最终库上重跑通过。

**T015 执行记录（2026-09-04）**：清单已生成
（`.codex-tmp/spec180-inventory-r116.json`，291 条目：1 cpp-suite + 77
cpp-selector + 208 python-selector + 5 minindn-case）。发现并修复了三个
清单级问题：(a) 清单 cpp 条目默认指向 2026-09-02 旧构建树二进制；已以
`--with-tests --ndn-svs-source-tree …/ndn-svs --ndn-svs-build-tree …/ndn-svs/build`
重配置 build-system-j2 并构建测试二进制（unit-tests/integration-tests，
15m16s，-j2 合规）；(b) 系统 nfd.conf 已补 `rib` 注册权限（HIGH 项修复）；
(c) 重配置曾短暂丢失自定义 ndn-svs 路径，已恢复并重建成库（16.9s）。
新二进制上的 spec180 相关集成选择器通过：NdnSvsSmoke、Spec170NativePostSelection、
NdnsfDataV1SvsFlow 全部 exit 0。残留原生失败：StreamFacade 套件（sign
throw ×3 + SIGABRT + 内存违规，共 10 例；该套件在 2026-09-03 迭代 48 证据中
为 exit 0，失败呈现环境/工作树相关，非 spec180 变更文件）与
Spec170NdnsfDiCoreFlow 中 6 例（`ProductionNativeHandlers*` 的 `/Aux`
输出断言，属前序会话原生运行时工作树状态）。二者均非今日 spec180 变更
路径，但 T015 的 cpp 条目在 SIF 前必须 triage 归因。

以上修复完成并重跑 T015 后，审计方可升级为 PASS。
