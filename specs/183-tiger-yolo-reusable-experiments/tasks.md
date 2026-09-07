# Tasks: Reusable TigerCluster YOLO Distributed Inference

**Input**: [spec.md](spec.md), [plan.md](plan.md), [profile contract](contracts/experiment-profile.md), [validation matrix](validation-matrix.md)
**Branch**: `TigerClusterExperiments`
**Status**: 2/17 tasks complete (T001 inventory and T003 focused component acceptance); IN_PROGRESS, no runtime PASS.

任务按行为闭环组织，每项包含必要失败例→实现→聚焦回归→证据，不拆成“写测试/改代码/跑命令”三个机械任务。正式 broad suites/实验必须等 T007；focused red/green 可在实现中执行。所有 Tiger 文件以下用完整仓库相对路径。

## Phase 1: Setup

- [x] T001 Freeze receiving inputs and unresolved inventory in `Experiments/TigerCluster/docs/yolo-reusable.md` and `specs/183-tiger-yolo-reusable-experiments/evidence/input-inventory.md`: reconcile four repository revisions against `Experiments/TigerCluster/development-handoff.lock.json`, delivery-vs-runtime source, nine native outputs, actual public YOLO APIs, model/package/oracle hashes, local/compute Apptainer and proposed partition/GPU/resources/capacity. Register exact build and test selectors and expected errors from current code; keep missing items WAITING_EXTERNAL_INPUT and historical failures intact. No large build/download or model job. Read-only local inspection first; bounded substrate probe only under the reviewed exception in plan.md.

T001 evidence: [input-inventory.md](evidence/input-inventory.md)。完成清点而非物理输入资格；三依赖精确对象、本地base、签名package/oracle仍缺，compute探测按T007后例外执行。现有host gate绑定旧tiny-onnx、User正常路径一次请求的问题已分派下列任务。

## Phase 2: Foundational

- [ ] T002 Implement phase-specific candidate closure in `Experiments/TigerCluster/runtime/yolo_profile.py` and `Experiments/TigerCluster/tests/test_yolo_closure.py`: test wrong/changed hash, incomplete inventory, path escape, stale receipt, unbound script/env and stage bypass before implementation; enforce I/R/E identities and invalidation with no self-reference, inputs-before-build/runtime-before-execution/dispatch-before-upload gates. Prove rejected inputs make zero build/upload/staging/sbatch calls with spies at the real command boundary. Depends on T001 interface inventory; gates may report missing physical input without pretending it is present.

## Phase 3: User Story 1 - Reusable Launch Configuration

T002 partial checkpoint：[内容完整性实现/21项focused回归](evidence/t002-integrity.md)。check_plane/check_chain已实现，但旧builder dispatch、真实receipt和实际命令边界零副作用测试未完成；保持unchecked，不作为正式资格。

T002追加接收审查修复：在既有`build-local-sif.sh`与host-gate owner增加显式Spec183 workload dispatch；校验实际YOLO日志/数值/源seal的receipt，保留Spec175 v1/v2。新增回归要求未知schema、错workload/源/receipt或失败证据使Apptainer调用数为零。T010产生真实receipt，T011消费；不得填M01假清单，也不得绕过已有source/ABI门。

**Independent Test**: deterministic resolved config from another cwd; fail-before-side-effects; two concurrent submit attempts cannot both launch.

- [x] T003 [US1] Extend reusable role/container lifecycle in `Experiments/TigerCluster/runtime/baseline.py`, `identities.py`, and `yolo_worker.py`, with `Experiments/TigerCluster/tests/test_yolo_runtime.py`: preserve CPU baseline behavior; support explicit GPU/env/mount/cwd mapping, isolated role identities/PIB, owned process groups and bounded peer/readiness checks. Add real short-lived parent/child cleanup tests, wrong cwd/port/shared PIB rejection and no host runtime injection. Reuse common primitives, not a copied supervisor. Depends on T001 and the T002 content-integrity interface; final T002 qualification is required at T007, not before implementation of its consumers. Implementation/focused acceptance: [evidence/t003-worker.md](evidence/t003-worker.md); actual workload wiring remains T004/T005, production audit T007.
- [ ] T004 [US1] Deliver strict profile and one entrypoint in `Experiments/TigerCluster/profiles/yolo-two-node.json`, `schemas/tiger-yolo-v1.schema.json`, `jobs/yolo/submit.py`, `jobs/yolo/run.sbatch`, and `tests/test_yolo_submit.py`: expose the five contract commands, resolve every field into actual argv/env, reject leftovers, bind immutable bundle, stage-scoped gates and allocate-once journal. Test atomic duplicate-run prevention and SUBMISSION_UNKNOWN recovery without resubmit; register case differences and walltime/timeout budgets. Fill real operational values before enabling submit; do not modify old `profiles/two-node.json`. Depends on T003.

## Phase 4: User Story 2 - Current YOLO Path And Independent Verdict

**Independent Test**: existing application APIs and independent oracle exercised by focused tests; no copying an expected tensor into the distributed output.

- [ ] T005 [US2] Wire the existing secure YOLO application into `Experiments/TigerCluster/apps/yolo.py` and `runtime/yolo_worker.py`, with `tests/test_yolo_application.py`: reuse `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py` and installed native Provider interfaces; map the four roles without fabricating ACK/Selection, verify permissions/current status and direct predecessor Data, enforce native input and independent oracle contract. Focused tests must catch lost structured assignment, stale epoch/input, missing backbone-to-head Data, duplicate/late output, and forbidden shared-filesystem activation shortcuts. Any Core/DI repair belongs to its existing source/tests and updates candidate provenance. Depends on T004.
- [ ] T006 [US2] Implement authoritative collection and failure oracles in `Experiments/TigerCluster/runtime/yolo_result.py` and `tests/test_yolo_result.py`: associate every request/attempt/plan/role/node/GPU/edge digest, validate numeric output via the existing contract, require all finite child exit0 plus controlled service shutdown/reap, and separate local/single-node/two-node/expected-rejection verdicts. Reject stale/forged PASS, missing role/response/CUDA, shape/class/tolerance mismatch, timeout-as-auth-rejection, nonzero worker, forced cleanup or corrupt output. Retain first failure and immutable reanalysis. Depends on T005.

## Phase 5: Design-Code Convergence

T005/T006追加调用次数约束：正常ACK-driven User一次只执行一请求。warmup/measured用独立User调用、request/attempt/输出目录及数值文件，Provider持续运行；focused test从生产入口证明实际调用次数，不能仅检查legacy sequential参数存在。

- [ ] T007 Audit production wiring before formal validation in `specs/183-tiger-yolo-reusable-experiments/evidence/design-code-convergence.md`: compare accepted spec/contract to actual submit→worker→application→Core/DI/Repo→collector and all effective fields through CodeGraph and exact source; register severity, owner and focused regression for each discrepancy. Review all wrappers/helpers and local/remote paths once as a closure, including scripts used by later substrate probes. Close only on PASS with zero controlling semantic/security/wiring/evidence gaps; actual changes reopen this task. Depends on T002–T006. This task is not satisfied by the planning audit.

## Phase 6: User Story 2 - Formal Local Qualification

**Independent Test**: current dependency build, real CPU multi-process graph and same-SIF app path. Record one evidence receipt per independently meaningful gate.

- [ ] T008 [US2] Build and qualify the locked dependency/native/Python closure using existing build owners and `Experiments/TigerCluster/docs/yolo-reusable.md`, recording `specs/183-tiger-yolo-reusable-experiments/evidence/host-unit.md`: clean ABI consumers in isolated build roots, system toolchain/Boost, at most `-j2` total per active build tree; record both Python extension imports, actual entrypoint checks, ldd/readelf/loaded hashes and all selected dependency/NDNSF/Tiger unit results. Preserve build failures; no stale incremental objects or manual PASS manifests. Depends on T007 and complete T001 inputs.
- [ ] T009 [US2] Execute real multi-process CPU integration in `Experiments/TigerCluster/tests/test_yolo_integration.py` and existing NDNSF integration fixtures, recording `specs/183-tiger-yolo-reusable-experiments/evidence/integration.md`: separate identity/bootstrap tests from a prepared authorized fixture; real signed messages, encrypted dependency Data, actual small ONNX execution and final oracle. Cover fresh Controller/epoch, denied role, wrong selection, activation loss/tamper and process cleanup. No mocked inference final PASS. Depends on T008.
- [ ] T010 [US2] Run bounded CPU MiniNDN YOLO through `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` with Spec183-owned case configuration/driver in `Experiments/TigerCluster/tests` and record `specs/183-tiger-yolo-reusable-experiments/evidence/minindn.md`: normal four-role graph plus registered permission/dependency-failure cases, request-to-response IDs/edge hashes, route snapshots and cleanup; freeze the actual same-source host qualification manifest accepted by the existing build gate. Do not rerun all historical campaigns or substitute echo. Depends on T009.
- [ ] T011 [US2] Build one complete local SIF via `Experiments/TigerCluster/adapters/slurm-apptainer/scripts/prepare-development-handoff.py` and `build-local-sif.sh`; record `specs/183-tiger-yolo-reusable-experiments/evidence/local-sif.md`: input gate, verified base and compute-matched Apptainer, container-built nine outputs, every packaged DSO closure, two extension imports/actual entrypoints, then exact-SIF local CPU YOLO. Keep model outside SIF, register final SIF/runtime identity and tests before promotion. Host/SIF hashes need not equal, but each must derive from its declared source/ABI. Depends on T010; reviewed short version-only allocation may precede build under plan exception.

## Phase 7: User Story 3 - GPU And Cross-Node Execution

**Independent Test**: same immutable runtime with actual allocated GPUs; final graph must contain genuine cross-node model dependencies.

- [ ] T012 [US3] Qualify promotion and allocated environment through `Experiments/TigerCluster/jobs/yolo/submit.py` and `runtime/yolo_worker.py`, recording `specs/183-tiger-yolo-reusable-experiments/evidence/tiger-environment.md`: audit final script/profile closure before side effects; verify local/project/staged SIF equality, compute Apptainer, CUDA/ORT, writable capacity, roles/cwd/mounts, NFD and bidirectional signed Data/permission readiness. No build on Tiger/login-node inference, no runtime path patching, zero Provider launch on mismatch. Depends on T011 dispatch receipt; per-allocation subset repeats in every subsequent job.
- [ ] T013 [US3] Run one bounded single-node GPU job with all four Provider identities via `Experiments/TigerCluster/jobs/yolo/submit.py`, recording `specs/183-tiger-yolo-reusable-experiments/evidence/single-node-gpu.md`: 1 warmup + 1 measured real request, observed CUDA per model role, independent numerical oracle and clean shutdown. This closes only SINGLE_NODE_GPU_PASS; resolve concrete failures and rerun affected earlier gates before advancing. Depends on T012.
- [ ] T014 [US3] Execute first normal two-node allocation from the unchanged normal case in `Experiments/TigerCluster/profiles/yolo-two-node.json`, recording `specs/183-tiger-yolo-reusable-experiments/evidence/two-node-first.md`: actual distinct hosts, A backbone/merge and B heads, 1 warmup + 3 measured requests, both directions of dependency edges and full per-request identity/backend/numeric/terminal closure. Stop on first failure and preserve partial results. Depends on T013.

## Phase 8: User Story 4 - Failure Recovery And Reuse

**Independent Test**: precise failure preserved; subsequent clean allocation reuses the exact normal configuration with no hidden repair.

- [ ] T015 [US4] Execute one bounded registered `negative-dependency` job through `Experiments/TigerCluster/jobs/yolo/submit.py`, recording `specs/183-tiger-yolo-reusable-experiments/evidence/tiger-negative.md`: after genuine Selection and before one required activation delivery, stop the owning Provider or use the registered fault Provider to suppress that edge. Expect exact missing-dependency/peer-failure boundary, no successful User result, no reselection, finite abort and full cleanup. Local mutation/unit gates cover other corrupt config/auth/false-PASS combinations; do not expand a remote fault matrix. Depends on T014 and matching fault harness receipt.
- [ ] T016 [US4] Reproduce normal distributed inference in a second fresh allocation via the same `Experiments/TigerCluster/profiles/yolo-two-node.json`, recording `specs/183-tiger-yolo-reusable-experiments/evidence/two-node-reuse.md`: unchanged E/profile/normal-case/SIF/model/harness/oracle, new run/identities/allocation, 1 warmup + 3 measured complete requests, no prior-run private/cache correctness dependency and no per-run large copies. Repeat complete allocation preflight, reconcile both normal runs and first failure logs. Depends on T015; changing behavior requires new identity and requalification, not counting the changed job as unchanged reuse.
- [ ] T017 [US4] Deliver reproducible configuration, operator guide and closure in `Experiments/TigerCluster/docs/yolo-reusable.md`, `Experiments/TigerCluster/README.md`, and `specs/183-tiger-yolo-reusable-experiments/evidence/closure.md`: check all fields and real commands from a clean checkout, offline recompute results, document fixed versions/qualification scope/cache location/first-failure guide, per-run warmup and measured latencies, exact code/config/SIF IDs, pending limitations and safe cleanup. Register full SC/FR evidence and close only if normal repeated runs plus required negatives meet criteria. Depends on T016; no performance superiority claim.

## Dependencies And Execution Strategy

实现顺序：`T001 → T002 内容完整性接口 → T003 → T004 配置/冻结/提交状态接口 → T005 → T006 → T004 五命令最终接线/验收 → T002 集成验收 → T007 → T008 → T009 → T010 → T011 → T012 → T013 → T014 → T015 → T016 → T017`。
T005依赖T004已有输入/运行接口，不要求其尚待T005/T006才能实现的实际argv和collect提前完成。T004保持unchecked，最终生产bundle必须包含真实应用/collector/run.sbatch；禁止生成占位文件让冻结检查假通过。T007仍同时要求T002–T006全部闭合，不减少任何资格门。
T002 的真实启动边界来自 T004，receipt 语义校验来自 T006；原先要求 T002 全部完成才实现消费者会造成循环依赖。允许先实现消费者不等于放开资格门：T002保持unchecked，T007必须同时验收T002–T006。T010才产生真实host receipt；之前仅可用明确标注的测试fixture验证拒错逻辑。
T001 中的缺 artifact/GPU 访问不阻止使用已知接口进行 focused 实现，但所有实际 build/run 的输入必须齐全；不能跳过 T007 或伪造后续 qualification。

T001–T004形成 profile/launcher 实现骨架；可操作 MVP 还要求 T006 判定器、T002集成验收和T007审计，不能把骨架作为可提交实验的版本。完整用户目标到 T017 才结束。
无 `[P]` 项：当前关键路径共享 profile/runtime 和真实资格，默认串行；可在任务内部并行只读盘点/离线测试，但不并行构建或自动派发 agent，不同时运行多个同 candidate/gate 实验。
合并了“测试→实现→局部验收→证据”机械链；各任务边界来自配置、生命周期、业务接线、判定、生产审计和独立环境验收。

## Current Checkpoint

2026-09-07 native观察组件：发现旧collector不兼容Boost PropertyTree bool/uint64字符串且hardcode example identity；新增严格decoder+真实Provider/PID/request/attempt/plan绑定校验，29 focused通过。不把日志声明当实际GPU/edge证据；ORT profile/物理GPU/依赖/cleanup/完整collector仍待做。见evidence/t006-native-observation.md。

2026-09-07 GRAPH_READY catalogueDigest source接通：YOLO builder既有验签/图/权重校验后保存签名body摘要，V3发出它并拒绝非法非空digest。真实LifecycleJournal文件→collector回归通过，连同候选映射/边界共34 focused通过；仍须最终collector对冻结catalogue做精确比较及真实native验证。T006保持unchecked。

2026-09-07 V3 candidate身份source修复：Yolo26Splitter重建catalogue条目的runtime candidate，精确digest唯一匹配后返回注册ID/digest；V3 lifecycle调用resolver，runtime计划/命名/digest不变。4项隔离production-kernel回归通过；真实adapter/planner未验证。另发现GRAPH_READY缺少catalogueDigest，writer未拒绝缺字段，须由verified catalogue补齐；T006仍未闭合。

最终复核：timestamp巨大整数负例补齐后25项lifecycle测试，完整focused集合480 passed / 45.24s（results/t006-lifecycle-r2/junit.xml）；未执行真实native/inference，候选身份缺口仍待修复。

2026-09-07 T006新增bounded lifecycle组件：按维护中journal的10个事件严格核对外部case/request/attempt/candidate绑定、字段/顺序/计数/digest/有限时间，拒绝duplicate JSON、symlink与超限输入；24 focused用例通过。保持LIFECYCLE_COMPONENT_ONLY，不能证明Provider执行/edge/cleanup。发现V3 planner的PLACEMENT_DECISION把candidate_digest写入candidateId，而prepared offer/numerical使用catalogue名称；尚未修复，必须追踪adapter候选与签名catalogue的映射后修正，不能放宽collector。T006/T007继续unchecked；见evidence/t006-lifecycle-component.md。

2026-09-07 T006数值重分析组件已实现：User显式opt-in保留0600/≤1MiB响应bin并记录digest，Spec183启用且绑定prepared candidate env；离线重算真实响应而非信任matched flag。共用纯NumPy tensor decoder，adapter exports按需加载使oracle/codec不依赖_ndnsf导入；operator NumPy锁定1.24.4。扩展focused455 passed（35.86s），包括实际producer函数/codec/数学/文件和独立import进程，但参考值为fixture、无模型/native/SIF/Tiger运行。T006完整lifecycle/role/node/GPU/edge/cleanup及operator/T007待完成；见evidence/t006-numerical-reanalysis.md。

2026-09-07 configure_network已接入实际NFD/nfdc启动链，生产nfd-ready/routes-ready：配置端口/两侧endpoint绑定、真实socket类型检查、有限管理子进程、剩余startup预算、peer失败、无覆盖receipt；管理进程借用空闲Provider HOME，不与NFD并发用PIB。route_commands显式接收appName/sync；旧baseline默认/group保留。7新增组件测试（OS进程/socket真实，NFD/nfdc为double），总419 passed（26.61s）。最终operator仍须绑定真实allocation并依次调用configure_network→start_workload→真实requests/collector；T005/T006/T007未完成，无native/SIF/Tiger运行。详见evidence/t005-nfd-network-setup.md。

2026-09-07 启动协调组件接入Controller/Repo/Provider/双向探测实际helper，原子且run/candidate/probe绑定barrier、共享startup预算、peer失败传播；必须两侧network receipt匹配后才启动Controller，完整Provider ready后返回RUNTIME_READY。用户明确Sync为`/<appName>/sync`：plan.applicationName→runtime.application_name→group，不从provider_prefix推导；错`/group`路由拒绝。10新增组件用例，总412 passed（26.13s）。NFD实际路由安装/最终outer worker、完整collector/T007仍未闭合，无SIF/Tiger运行；见evidence/t005-startup-coordination.md。

2026-09-07 双向签名Data readiness组件已实现：apps/yolo_network.py，两rank并行、同一新probeId、真实RSA验签、精确name/payload、独立有限窗口；复用finite-role进程/HOME管理，探测不挂载model/GPU，退出后允许真正Provider启动。身份从prepared plan传入，不假设等于role名字。父层校验精确回执与退出，正式operator仍须同时接受两个方向；harness清单更新15文件。13项新增组件测试，总402 passed（25.00s），其中加密真实但Face是内存double。实际NFD/SIF/Tiger仍NOT_RUN；T005/T006/T007未闭合。详见evidence/t005-network-readiness.md；下一步完整worker协调/启动barrier及请求collector，不扩大实验。

2026-09-07 Repo readiness组件已接入真实NetworkDistributedRepoClient.capability()接口，normal/FirstResponding、禁用Targeted fallback；有限User独占HOME且不挂载模型。启动probe有独立nonce/回执/调用目录、monotonic预算及外层进程deadline，拒绝过期/异Repo/非零退出/symlink证据，cleanup完成才写READY。18项新增测试，总389 focused（22.60s）；native RPC仍为测试double，真实SIF/NDN运行NOT_RUN。跨节点readiness、完整operator和T006仍待完成；T005/T007保持unchecked，不提交Tiger作业。

2026-09-07 Controller publication readiness接入真实receipt共享校验器，MiniNDN和Tiger复用runtime/yolo_result.py，拒绝重复artifact行；宿主校验不导入SIF-only路径。Provider readiness绑定精确identity/单role READY，source已确认在权限安装之后，不代表model/CUDA ready。8项新增测试，总371 focused（22.30s）。yolo_result.py仅完成publication检查，T006完整collector仍待实现；正式operator、Repo/跨节点readiness及T005/T007仍未闭合。

2026-09-07 NodeRuntime.from_preparation已消费receipt/inventory，在构造前绑定mode/rank/role/run输出，并在每次role启动/User调用前重查；不在100ms存活轮询中hash。3项真实文件边界测试，总363 focused（20.70s）。正式operator仍须接用该factory并独立验证SIF/gates；直接constructor仅低层生命周期测试用途。T005/readiness/T006/T007未完成。

2026-09-07公开准备清单已写入receipt：精确文件集合/逐文件hash，拒绝未知目录、特殊文件、symlink、private PEM和缺失；verify_preparation绑定外部receipt hash及run/candidate再重算。10项文件fixture测试，总360 focused（20.85s），最终类型收紧后10项再次通过。外层operator/worker消费仍待接线，完整SIF准备及身份/模型资格未执行；T005/T007仍未完成。

2026-09-07内部prepare CLI/固定输入descriptor已接通，共享容器启动器只允许离线准备挂载/inputs，禁止普通worker/网络/GPU带入私有输入。处理Apptainer预建空root HOME，仍拒绝旧内容；7项边界测试通过。正式submit.py prepare资格入口、public manifest inventory/readiness/T006仍待闭合，完整native准备未执行。详见t005-public-recipients.md。

2026-09-07内部prepare编排已接入apps/yolo.py：固定空挂载、输入hash、原adapter签名图校验、真实issuer与各类材料、原Y-B policy/Repo权限/native plan和catalogue batch生成器。5项配置投影测试通过，总343 focused（20.50s）。完整prepare尚未实际执行，最终CLI/public manifest绑定/readiness/T006仍未闭合；不能根据代码接线宣称PREPARED或T005完成，T007须审调用边界、T008/T011实跑。

2026-09-07准备路径审查发现Controller会向只读/config写publication receipt；已增加显式receipt参数，Spec183改写/output/runtime-publication-receipt.json，输入保持只读且拒绝覆盖。338 focused通过（20.61s），文件行为测试使用真实函数体+模拟传输，不代表签名发布通过。Controller源码变化要求新source seal/SIF。准备主流程及readiness仍待完成，T005 partial。

2026-09-07 pinned trust导入组件完成：保留registry原始bytes和catalogue/model/authority公钥，校验candidate digest、public hash、epoch及authority私钥匹配，只将私钥放User HOME。`authority.pub`是native定位别名，不再要求重写registry的publicKeyPath；此前checkpoint该措辞已被修正。6项真实crypto/加载测试，总337 focused（21.89s），类型收紧后6项再次通过。尚需最终prepare接线、完整模型签名验证、runtime publication/readiness与T006；T005仍partial。

2026-09-07 offer准备组件已实现：四个独立Ed25519签名密钥，实际certificateName/keyLocator、Provider/service、candidate和Trust Schema绑定；复用独占写入和HOME lease。5项新增测试，总331 focused（20.04s）。尚未接最终prepare或真实ACK验证；下一步authority/catalogue输入认证和完整准备/readiness，后续T006。T005仍partial，详见t005-public-recipients.md。

2026-09-07身份issuer现从实际证书Data记录certificateName/keyLocatorPrefix，避免offer policy猜测名称；5项wire解析测试通过，总326 focused（21.32s）。这是证书结构解析，不是签名认证/实际issuer通过。下一步offer密钥及policy准备接线、模型/authority认证和readiness。T005仍partial，见t005-public-recipients.md。

2026-09-07真实recipient生成组件已加入既有identities.py：每Provider独立密钥/私有map，User独立requester seed和public map；独占写入及HOME lease，拒绝覆盖/身份重复。4项真实crypto测试通过，总321 focused（21.13s）。尚未接最终prepare；模型/authority/offer材料认证、readiness及T006仍待实现，T005不关闭。详见t005-public-recipients.md。

2026-09-07 native recipient启动接线已按真实C++环境变量完成，私钥map精确限定单Provider自身HOME；User和Provider共用`/config/contracts/trust-root-registry-v1.json`，publicKeyPath契约为`contracts/authority.pub`。新增6项拒错，317 focused通过（20.91s），证据追加于t005-public-recipients.md。下一步生成并认证这些真实材料及readiness；不能把fixture路径检查当作native grant通过。T005仍partial。

2026-09-07公共recipient接线：User支持public-only map，Spec183启动强制显式protected epoch及自身requester/authority密钥；311 focused通过。真实User seam新增两项测试因缺少`_ndnsf`在setup失败，未证明grant集成通过；T008须完整重跑。见[evidence/t005-public-recipients.md](evidence/t005-public-recipients.md)。下一步signed准备/registry布局/native Provider recipient/readiness，再T006；T005/T007保持未完成。

2026-09-07控制面接线：apps/yolo.py已复用Controller/Repo入口，role-local policy/store、显式capacity、bounded JSON拒错；见[evidence/t005-control-launch.md](evidence/t005-control-launch.md)。新审查发现现有User默认plaintext-v1且旧protected seam读取Provider私钥map，未满足Spec183隔离；下一步优先实现公共recipient key map并绑定显式protected epoch/requester/authority/native recipient输入。沿用既有可信in-process authority边界，不新增网络授权服务，不共享Provider私钥。完成后继续signed准备/readiness/T006；T005及T007保持未完成，不放行Slurm。

2026-09-07源码发现：YOLO声明CPU/CUDA备选，但_v3_role_specs只取首项，GPU-only模型Provider或CPU Merge无法同时满足。已在原DI coordinator保留明确的ONNX CPU/CUDA family后再由ACK选择；9个kernel测试复现/修复并保留资源/角色/CPU-only负例，见[evidence/t005-backend-selection.md](evidence/t005-backend-selection.md)。T008必须在原生绑定构建后以`SPEC183_REQUIRE_NATIVE_PLANNER_IMPORT=1`重跑该文件，禁止AST模式代替完整import。该源码变化使旧runtime/source seal失效；handoff交付SHA只作provenance，新SIF必须含修复。T005仍partial，Controller/Repo准备和T006未完成。

2026-09-07追加User接线：apps/yolo.py复用真实one-shot入口，NodeRuntime顺序持有User HOME并保留Provider，修正裸requestId与错误output预览路径。两节点1+3、单节点1+1均经进程边界聚焦测试；首失败停止。见[evidence/t005-user-schedule.md](evidence/t005-user-schedule.md)。T005仍partial；下一步准备signed material、Controller/Repo和真实permission/catalogue readiness，再T006。不得把测试validator替代正式collector；negative-dependency暂拒绝普通scheduler。

2026-09-07追加：T005 partial已将安装版native Provider启动参数接入NodeRuntime，四角色CPU/GPU与启动前拒错共15项新增测试；完整Tiger focused集合 **275 passed in 16.18s**。见[evidence/t005-native-launch.md](evidence/t005-native-launch.md)。下一步仍为真实应用coordinator、Controller/Repo/User准备与安全readiness、逐请求执行，然后T006；T005未完成，不放行T007/Slurm。

2026-09-07：T001/T003完成（2/17），T004 partial新增脚本bundle freeze/verify并接入dispatch真实入口，最新 **260 passed in 17.17s**，见[evidence/t004-harness.md](evidence/t004-harness.md)。下一步转T005实际应用/argv/cache/安全模型传输，再T006，回填T004完整五命令和T002资格。生产bundle缺真实应用/collector/run.sbatch，不制造占位文件；不新增Provider模型旁路挂载。三依赖精确commit已隔离接收，原工作树未改；source封装、签名模型包和本地base仍待完成。T007及全部正式环境门未通过，无构建/上传/Slurm/模型执行。
