# Failure Log and Evidence Index

## 2026-09-07 — Standalone transport test lacked its canonical import root

- Symptom: targeted receiver tests stopped during collection with
  ModuleNotFoundError: runtime; no receiver test executed.
- Cause: the new test lacked the import-root setup supplied by other tests
  when a larger group runs.
- Fix: explicitly insert the canonical Tiger root. Initial focused.xml retains
  the failure; final.xml records 15 passing component checks.
- Evidence: results/spec183-transport-receiver-20260907; separate real two-file
  Tiger receive/reuse documented in t004-transport-receiver.md.
- Lesson: targeted tests must collect independently; do not expand a suite
  merely to get another module's sys.path side effect.

## 2026-09-07 — Sender submit selected a receiver-only Python path

- Symptom: `_enter_frozen` selected the configured Tiger operator Python for
  local submit before reaching receiver staging/site checks; that absolute
  executable need not exist on the sender.
- Cause: submission coordination and allocated execution used the same
  interpreter-selection branch after adding runtime.operatorPython.
- Fix: submit coordinates with the invoking host's verified interpreter;
  receiver checks the configured batch interpreter before sbatch. Run/rank
  keep the cluster selection. Profile/frozen dependency binding stays intact.
- Validation: 68 affected component checks passed; retained JUnit under
  results/spec183-submit-origin-20260907. No actual remote/model execution.
- Lesson: a configured remote physical locator must not become a local
  executable before crossing an explicit host boundary.

## 2026-09-07 — Spec183 preflight boundary test expected an obsolete missing-script error

- Symptom: the first regression after adding the real two-phase Spec183
  preflight expected `SPEC183_PREFLIGHT_MISSING`.
- Cause: the preflight script was now present, so the minimal fixture correctly
  failed earlier with `SPEC183_HARNESS_NOT_SEALED`.
- Fix: rename the test to assert rejection of an incomplete source seal and
  retain the zero-Apptainer-call assertion.
- Lesson: when a fail-closed gate becomes real, update the boundary test to the
  earliest authoritative rejection reason; do not preserve an obsolete
  placeholder-absence expectation.

## 2026-09-07 — Pure YOLO reanalysis imported native runtime eagerly

- Numerical reanalysis test collection failed while importing adapters.yolo.reference: adapters package initializers eagerly loaded runtime contracts and the missing `_ndnsf` extension. The traceback's partial-initialization wording did not mean the NumPy oracle itself required native code.
- Preserve all public adapter export names but resolve their owners lazily. Extract the existing YOLO native tensor decoder to a shared NumPy-only module; both application and reanalysis use it. A fresh subprocess proves pure imports do not load ndnsf/_ndnsf. Native API compatibility is still a separate T008 gate.
- The same audit found Spec183 did not pass candidate environment fields into numerical evidence and retained no response for reanalysis. Bind candidate fields to prepared metadata and opt in to bounded private response evidence; recompute instead of trusting PASS.
- Evidence: Spec183 evidence/t006-numerical-reanalysis.md; expanded focused455 passed, no native/model/remote qualification.

## 2026-09-07 — Network probe negative test must reject for the intended reason

- The first in-memory Face fixture passed the corrupted-signature negative under a broad Exception assertion, but a stricter check exposed an incorrect ValidationFailure constructor (TypeError, not signature rejection).
- Fixed the fixture to use the installed python-ndn constructor and require the exact expected exception class for signature/certificate/payload/timeout cases. Actual RSA verification is retained; no production protocol failure was found here.
- Lesson: a negative security test must establish the rejection reason, not merely observe an exception. Evidence: Spec183 evidence/t005-network-readiness.md, final full focused 402 passed.

## 2026-09-07 — Spec183 protected grant material absent from launcher closure

- Read-only audit of the maintained User found missing SPEC181_PROTECTION_EPOCH silently selects plaintext-v1, and its protected branch reads Provider private-key paths to derive public recipients. That historical fixture arrangement is incompatible with role-private HOME mounts.
- Status: controlling wiring gap, not fixed by this checkpoint's Controller/Repo argv implementation. T005 must use a public recipient map, explicit epoch and per-owner private locators, and bind native Provider decryption inputs. Keep the existing explicitly trusted in-process authority role; do not claim process isolation it does not implement or invent a new authority service within experiment scripts.
- Evidence: Spec183 evidence/t005-control-launch.md. No model/Slurm run occurred; release gating remains closed.

## 2026-09-07 — YOLO V3 backend alternatives collapsed before ACK placement

- Actual adapter allowed CPU and CUDA, but coordinator initial RoleAssemblySpec retained only the first backend. Pure production-kernel reproduction rejected CUDA BackboneNeck with CPU-first ordering and CPU Merge with CUDA-first ordering despite complete role coverage/resources.
- Normalize only the explicitly declared ONNX CPU/CUDA pair to its portable family until the existing signed-offer strategy selects a concrete backend/device. Keep single-backend requirements strict. Tests cover both orders, mixed/all-CPU placement, missing role, insufficient memory and wrong engine.
- Host has no current `_ndnsf` yet; ordinary planner import failed at the missing binding. Focused tests use verbatim AST-selected production kernels with real SDK types, not a fake native module. T008 must repeat with SPEC183_REQUIRE_NATIVE_PLANNER_IMPORT=1 after rebuilding. This is not CUDA/model evidence. New source seal/SIF required; see Spec183 evidence/t005-backend-selection.md.

## 2026-09-07 — Spec183 User preview incompatible with real entrypoint

- Source inspection found run-plan requestId was a bare hash while the maintained ACK-driven User requires an absolute NDN name. The plan also predicted a different output directory from the role-isolated mount. Corrected both and added command/plan assertions and bad-ID/output negatives.
- The User's APPClient always writes generated policy; direct reuse of read-only public policy as its output would fail. It now writes under its exclusive invocation output and consumes the immutable case config separately.
- Real one-shot Users require separate processes; legacy sequential flags do not repeat the ACK-driven path. Added the actual scheduler component and shared finite wait service/peer checks. Focused tests use synthetic model/config and OS-boundary substitution, not inference evidence. See Spec183 evidence/t005-user-schedule.md.

## 2026-09-07 — Spec183 native launcher acceptance scope

- Implemented the previously missing actual native Provider argv path in NodeRuntime rather than copying the old Spec180 renderer's hardcoded identities/private model-root layout. Role keys belong in isolated HOME, generated public configuration in /config, and model assembly cache in each /output.
- The CPU test extension initially created new role directories without PIB files; existing validation rejected all four cases. Completed the synthetic fixture rather than weakening role isolation. This is a test-fixture error, not a production SIF failure.
- 275 focused tests pass; subprocess-boundary substitution proves command wiring and rejection/cleanup, not model execution. T005 coordinator/readiness/User flow and all formal gates remain pending. See Spec183 evidence/t005-native-launch.md.

## 2026-09-07 — Spec183 frozen harness bounded verification and dependency order

- Initial harness verification recursively enumerated unexpected directories before rejecting them, risking needless traversal of injected large model/result trees. A real Python audit-event test reproduced the traversal; verification now scans only the finite registered directories and rejects an unknown subtree immediately. Patching os.scandir alone did not instrument Python3.8 pathlib's cached accessor and was replaced by the actual audit observation.
- T004 complete CLI/frozen production inventory depends on the T005 application and T006 collector. Requiring T004 fully closed before implementing those consumers would be another implementation dependency cycle. Use the implemented T004 interfaces to implement consumers, then close T004/T002 before T007; never fill missing production files with stubs or relax qualification.
- Evidence: Spec183 evidence/t004-harness.md, 260 focused tests. Small synthetic harness integrity is not source approval or model/runtime PASS; no Slurm/model execution occurred.

## 2026-09-07 — Spec183 unnecessary Provider model staging

- Source audit found the new NodeRuntime required a model_artifacts mount that the actual YOLO native Provider never consumes. The existing User publishes encrypted graph/weights/root over NDN; the Provider assembles them in its own artifact cache. Designing a role-only filesystem projection would add a second unnecessary deployment path.
- Removed the new worker-only parameter/mount and corrected the current contract/plan; retain generic baseline helpers. Four role/rank launch regressions verify no out-of-band model mount. 239 Tiger tests passed; real application wiring remains T004/T005, and no SIF/GPU qualification is claimed.
- Lesson: trace producer→artifact reference→native consumer before adding deployment directories. Passing fixtures for a proposed mount do not prove the production application uses it. See Spec183 evidence/native-model-route.md.

## 2026-09-06 — Spec183 CLI cold-import audit and journal recovery validation

- Fresh-interpreter auditing found jsonschema imports uuid/platform on Python3.8, which runs the read-only `uname -p` helper and opens `/dev/null`. The initial test overclaimed no subprocess at all. Scope the audit to forbidden launch/network/filesystem mutations, permitting only that exact standard-library probe; no experiment operation is exempted.
- Journal mutation tests found unhashable state values escaped as TypeError and RUNNING with no jobId was accepted by the reader. Strict state type and job/state consistency now reject both without rewriting the record. Checking only a new reservation would have masked the second defect behind ACTIVE_RUN; the regression reads the corrupted active record directly.
- Real process and injected-I/O tests establish one reservation winner, crash/unknown no-resubmit, terminal preservation and fail-closed directory-fsync ambiguity. These are local filesystem component evidence, not proof of Slurm submission, remote locking or model qualification. See Spec183 evidence/t004-cli-journal.md.

## 2026-09-06 — Context hook output overflow during Spec183

- Symptom: a context hook emitted a truncated report originally exceeding one million tokens; the preceding tool result was lost from the conversation although its file edits persisted.
- Finding: this is a host/context-output incident, not a failed SIF or Slurm job. The exact hook-generation cause remains unverified; passing the Context Mode health guard does not establish bounded hook output.
- Recovery: verified git status and the three new T004 files before continuing, reran only the finite tracer test, and resumed from repository-backed tasks/evidence. Did not replay writes blindly, purge stores, or restart an experiment.
- Status: execution checkpoint recovered; host output containment is unresolved and must not be called repaired. Keep tool results bounded and use durable checkpoints when another overflow occurs.

## 2026-09-06 — Spec183 worker ownership, path aliases and repeated TERM

- Symptom: focused production-launcher tests found two workers could acquire the same Provider HOME, missing/misbound directories could launch, role-output symlinks could write into the bundle, and a second TERM killed the cleanup owner (outer process exit -15). The old issuer supported only the CPU role list, while YOLO app source requires explicit state-root and ORT-profile locations.
- Fix: NodeRuntime combines the existing shared launcher/Processes with real HOME leases, role-scoped state/model/output bindings, bounded incremental marker observation and NFD port/config validation. Reject directory overlap/symlinks; protect bounded teardown from repeated TERM/INT. Parameterize the original issuer's derived identity map while preserving CPU defaults and public-only peer certificates.
- Evidence: Spec183 evidence/t003-worker.md; 171 focused tests passed in 5.87s, with real processes/locks/signals and a declared fake Apptainer boundary. Actual SIF issuer/CUDA/YOLO remains NOT_RUN; T004/T005 must wire the component and T007 must audit it before formal gates.
- Lesson: per-role paths and source-derived env fields must be consumed by the actual launcher, not just listed in a profile. Never stage a whole oracle-containing package into a Provider; derive role-only model projections from the frozen inventory.

## 2026-09-06 — Spec183 cleanup budget, identity layout and finite deadlines

- Symptom: per-child shutdown could multiply the job cleanup budget; finite application had its own cleanup path and accepted unbounded deadlines. Identity helpers failed import on host Python3.8 (`list[Path]`), and distinct role directories alone did not rule out shared PIB/private-key inodes.
- Cause: CPU-era helpers lacked a group deadline and prepared-layout gate; duplicated lifecycle code and eagerly evaluated annotations escaped earlier tests.
- Fix: shared monotonic-budget close with retained unreaped owners and continued cleanup after OS errors; finite entrypoint reuses it and validates deadlines/cwd before launch. Add deferred annotations and read-only inode/path layout checks, called by the original issuer before peer certificate import. Focused red/green and final 115 passed in 4.00s are recorded in Spec183 evidence/t003-lifecycle.md.
- Limits: filesystem fixtures do not prove crypto/identity correctness, and shared-helper tests do not prove the planned YOLO worker is wired. T003 remains partial, actual issuer/SIF/GPU checks remain pending. No runtime gate was relaxed.

## 2026-09-06 — Spec183 gate dependency cycle and launch isolation gaps

- Symptom: T002 final acceptance needs the T004 external command boundary and T006 result validator, but the task chain required T002 complete before implementing either. Focused launcher tests also exposed missing GPU/model/cwd arguments, unchecked optional bind paths and ambient GPU settings (25 failed / 1 passed before repair).
- Cause: implementation interfaces and final qualification were conflated; the shared CPU-only launcher had not yet implemented the new GPU contract.
- Resolution: allow T003–T006 implementation from T002's integrity interface, then close T002 integration before T007. Extend the original shared launcher with explicit single-device selection, fixed read-only artifact mount, all-mount path checks, filtered environment and child cwd. Focused regression: 84 passed in 2.60s; see Spec183 evidence/t003-launch.md.
- Remaining: T002/T003 stay unchecked; the real worker, aggregate cleanup budget, receipt validator and launch gates are not qualified. Do not manufacture receipts or treat argument tests as GPU execution.

## 2026-09-06 — Spec183 integrity preflight ambiguous and blocking inputs

Focused red/green found inventory omissions, unknown fields and escaped paths accepted by the initial integrity tracer bullet (7 failures), then an unimplemented ancestor chain (1 failure). A separate FIFO-manifest probe timed out after 2 s: ordinary open could block even before validation. Strict metadata/path/JSON checks, recomputed parent identities and nonblocking regular-file opens resolve these focused cases; final 21 tests pass. Full T002 launch/receipt gating remains pending. See `specs/183-tiger-yolo-reusable-experiments/evidence/t002-integrity.md`; these fixture passes are not build/GPU qualification.

## 2026-09-06 — Spec183 pre-execution workload and repetition gaps

- Symptom: receiving-source audit found `build-local-sif.sh` only accepts Spec175 tiny-onnx host qualification, while Spec183 requires actual YOLO qualification. The maintained ACK-driven YOLO User also executes one request despite exposing legacy sequential options.
- Cause: old workload-specific validation remained inside the reusable builder; command-line availability was not equivalent to active-path consumption.
- Resolution: recorded the exact source contracts and assigned T002 explicit workload-specific receipt dispatch, T010 real YOLO receipt production, and T005/T006 separate request invocations with independent evidence. These code repairs remain pending; no fake M01, timeout change or cluster job was used.
- Lesson: verify both the builder's accepted evidence schema and the application's actual request count before expensive execution. Inventory: `specs/183-tiger-yolo-reusable-experiments/evidence/input-inventory.md`.

## 2026-09-06 — Spec183 receiving-context authority drift

- Symptom: after receiving Experimental, `.specify/feature.json` selected Spec182 while local `AGENTS.md` still referenced Spec179; project Context Mode health passed but strict active health failed.
- Cause: tracked feature pointer and ignored local agent instructions were not synchronized by the branch update.
- Resolution: for the user-requested Spec183, update both references, index the maintained spec/plan/tasks, and verify strict active health. Preserve older Spec/GSD status as history; Spec183 runtime remains NOT_RUN.
- Lesson: project-health PASS is not active-Spec authority. After receiving another machine's branch, verify the pointer and local managed block together before resuming work.

## 2026-09-06 — Delivery-only scope correction

用户明确指出本轮任务仅为交付，编译与测试由另一台机器负责。此前本机ABI消费者验证属于超出范围的扩展；立即停止R4 owned构建进程组2531869，不再启动NDNSD构建、unit/integration、Python扩展验证或MiniNDN。R1/R2中断及R3普通Provider构建记录保留，不能外推完整验证PASS。后续构建/测试均TRANSFERRED，不作为交付阻塞项；当前源码包/definition/依赖锁/skills与GitHub发布已完成，见source handoff。

## 2026-09-06 — Waf interrupted signature persistence

R2日志证明R1超时后Waf未保存task signatures，实际从1/318重新编译，不能称为仅续编剩余对象。停止重复R2（SIGINT exit68，78.516s）并确认编译子进程退出，保留同一raw root的`build-r2/boundary.json`。R3以原配置、`-j2`、3600秒上限只选择缺失的`di-native-provider`及必要依赖；R1在同一fresh树已经成功链接的Core/unit/integration/应用保留逐目标证据。最终需补齐全部交付目标并运行测试，不将任何中断轮次记PASS。

## 2026-09-06 — Fresh ABI build runner time limit

新SVS/NDNSD闭包消费者fresh build R1在299/318触发执行器1800秒上限：exit124，1800.081s，`TIMEOUT_AT_RUNNER_BOUNDARY`。没有compiler error，Core/unit/integration及部分应用已链接，仍有native-provider对象未完成；不能把未完整构建记PASS或解释为协议失败。独立验证树 `/home/tianxing/NDN/ndnsf-svs-abi-20260906` 下 `.codex-tmp/svs-abi-20260906-r1/build-r1/` 保留receipt、boundary和完整log；确认无遗留编译进程后以 `build-r2`、3600秒有限上限继续同一fresh `build-abi`，配置和`-j2`不变。

## 2026-09-06 — Source handoff tooling resolved

最终交付工具/模板/旧builder fixture统一R5 **28/28 PASS**；五项共享skill与接收说明链接/语法通过。真实四库包生成、搬迁后verify、固定base摘要及definition render PASS。R1/R2的生成物/离线VERSION.info、canonical workload与历史host-gate fixture边界均已定位并修复，原日志保留。详见 [source handoff checkpoint](../Experiments/TigerCluster/docs/source-handoff.md#checkpoint)。NDNSF新SVS/NDNSD ABI闭包的fresh编译与运行验证仍待完成，不宣称SIF或Tiger PASS。

## 2026-09-06 — SVS offline metadata and transitive ABI closure

真实归档R2在 `LOCAL_SIF_DEPENDENCY_SOURCE_MISSING:VERSION.info` 停止（exit1），raw `.codex-tmp/source-handoff-20260906/package-r2.log`。SVS的该文件由Waf生成且被Git忽略，本机残留版本仍指向旧commit。改为从固定源码的VERSION/GIT_TAG_PREFIX和git describe生成归档内元数据，记录派生来源，不修改源checkout。同时静态查到NDNSD自己构造SVSPubSub，旧NDNSD二进制也是ABI消费者；将其干净源码及pkg-config路径修复纳入锁定和fresh重建，不复用base中的旧库。

旧build-record fixture R2进一步在 `SPEC175_WORKLOAD_NOT_SEALED:Experiments/TigerCluster/jobs/spec175/workload.json` 拒绝，raw `.codex-tmp/source-handoff-20260906/source-handoff-build-record-r2.log`：兼容alias与canonical路径不一致。fixture按canonical封装，真实sealer同时保留legacy镜像路径及canonical身份，生产门保持不变。

## 2026-09-06 — Source archive cleanliness boundary

真实交付包R1在 `HANDOFF_SOURCE_UNTRACKED:examples/example-trust-anchor.cert` 拒绝，未创建bundle：开发依赖checkout带有未跟踪生成物，HEAD与tracked clean不足以证明归档内容。保留原目录，改为三库全部使用精确commit的全新detached checkout准备R2，不放宽sealer的未跟踪源码检查。这是输入来源失败，尚未运行容器编译。

## 2026-09-06 — Source handoff tool fixtures

交付工具统一检查R1为19 PASS / 5 FAIL，原始 `.codex-tmp/source-handoff-20260906/tool-checks-r1/output.log` 保留。五项旧 `test_build_local_sif_record.py` 都在 `HOST_GATE_WORKLOAD_SEED_MISMATCH` 提前退出：fixture引用历史真实G3清单，但运行时校验当前workload。未到达所测definition/label/source边界，不是新SIF构建失败。修复测试为独立临时fixture，保留生产门与负例；不更新历史资格清单冒充当前结果。

模板首轮静态检查曾因开头注释被既有boundary parser识别成第三stage而失败；说明移入builder头之后。随后静态复审发现wheel锁只检查非空会漏掉离线python-ndn依赖，现要求五个固定wheel输入并补缺项拒绝。模板原始R1/R2日志由本轮 [source handoff](../Experiments/TigerCluster/docs/source-handoff.md) 记录；这些静态/fixture失败均未执行Apptainer。

## 2026-09-06 — Merge validation resolved

静态修复后 full unit 759/759、GDB full integration 154/154、current Python 2171 passed /22 skipped；MiniNDN用户撤销、仅新增授权、Provider撤销全部 PASS。PATH启动失败在独立 R2 修复，最初日志不覆盖。Provider场景主动 SIGINT 后重启的旧进程 exit -2，其余应用 exit0。完整身份和范围见 `specs/182-native-di-python-bindings/evidence/merge-validation-20260906.json`；不能把 current Python 范围或三个网络场景外推为历史全套/181最终qualification。

## 2026-09-06 — MiniNDN launcher PATH boundary

合并验证 `minindn-user-revocation-r1` 在 0.644 s 退出1，首边界为 Mininet 启动器找不到 `ifconfig`；编译 PATH `/usr/bin:/bin:/usr/local/bin` 遗漏系统网络工具目录。拓扑/协议尚未运行，不能解释为撤销失败。系统 `/usr/sbin/ifconfig` 已确认存在；MiniNDN root PATH 增加 `/usr/sbin:/sbin`，保留编译器绝对路径约束。原始 `.codex-tmp/merge-20260906/minindn-user-revocation-r1/output.log` 保留；下一轮使用新目录。

## 2026-09-06 — Full integration after static fences

`integration-static-r1` 仍运行时已发现两个首边界：`Spec175NativeTinyOnnxExpiredDeadlineCleansProviderState` 的 provider failure count 不符，以及 `ProductionNativeHandlersRunD2h212ToCompleteOracleResponse` 缺一个角色/最终 oracle。保留本次完整 GDB 日志，先对照新增排队 fence 与原始 deadline/cleanup 状态更新，不能直接放宽 timeout 或删除拒绝断言。定向生命周期、证书撤销及 Controller45项通过不能替代该全模块结果。

## 2026-09-06 — Static repair cross-review lifetime finding

Build static-review R1 主动 SIGINT（exit68，154.920 s）。交叉复审发现新增 handler `!current()` 分支在 Provider 析构排空队列时仍向 Face 投递裸 `this` 的失败回调；析构后 dispatch 存在 UAF。停止当前构建，先在投递前和回调内检查共享 stopping token，并补 queued-handler 析构回归；不得把中断视为构建通过。原始 `.codex-tmp/merge-20260906/build-static-review-r1/` 保留。

## 2026-09-06 — Static review and full-module recount

NFD 定向 R1 进一步定位：提供私有 NFD 后，首边界变为 PUBPARAMS readiness timeout（10.418 s），因为 Controller 主 Face 是 DummyClientFace，不能与独立真实 probe Face 经 NFD 往返。不是仅缺守护进程。12处 policy fixture 启动改为已有 test-access 调用真实 `registerInterestHandlers()`，不设置 ready、不替换策略/签名处理；完整 `start()` 仍由13项 standalone readiness 和真实 NFD 测试覆盖。原始 `.codex-tmp/merge-20260906/integration-nfd-preflight-r1/`，NFD `/tmp/ndnsf-int-ntroyrk7/`，owned child cleanup=0。

Python import isolation R1 **43 passed / 3 skipped**（12.630 s），CLI loader 恢复 `sys.path` 后与 YOLO 联合执行通过；包含路径保持的新回归。

完整 module 日志纠正先前只统计部分 suite 的摘要：integration R3 为 **140/154 PASS、14 failed**，R4 为 **142/154 PASS、12 aborted**。其中 11 项是 Controller fixture 的 Face 尝试连接不存在的私有 NFD socket；另 1 项 certificate revocation 的 Controller 大写 digest 与 User/Provider 小写 digest 不匹配，不能归类为时间波动。原始目录 `integration-r3`、`integration-r4` 保留。MiniNDN 前先修复并重新验证。

当前 Python R1：2158 passed / 8 failed / 22 skipped；八项 YOLO 导入失败，独立 YOLO 19 项通过，正在检查联合执行时的 lazy import 首异常。原始 `.codex-tmp/merge-20260906/python-current-r1/output.log` 保留。Context Mode 的 `session-events` timeline 恢复查询被 guard 拒绝，继续以仓库、原始日志为准。

静态审查发现 User identity prefix retry 捕获局部 `onFail` 引用，构造结束后的失败回调存在 use-after-free；修复后须覆盖延迟重试。

## 2026-09-06 — D2h predecessor boundary / integration R3

定向 R2 使用了 Boost.Test 不接受的逗号连接完整路径，exit 200，未执行协议用例；原始 `d2h-regression-r2` 保留，改用同一 suite 下的 `ProductionNativeHandlersRunD2h*` selector 重试。

完整 R3：90/92 PASS，exit 201，183.621 s，无崩溃。Trace R1 首次失败为 `NDNSF_DATA_V1 HMAC verification failed`：compact segment 的认证预算原由 capability 生成，接收方错误地用可更严格的 Selection edge deadline 恢复 AAD。恢复 capability 的原始认证字段，同时保留 edge 对实际取数的 deadline 限制；D2h 121/212 既有测试正好覆盖两者不同的情况。

R3 已通过原崩溃的 targeted-stream 阶段；D2h 121/212 仍分别只观察到第一阶段 1/2 个角色，未得到完整 oracle response。按后继准入、依赖取数、scope-key 解密顺序使用独立 trace 定位；不扩大超时或删除 oracle 断言。原始 `.codex-tmp/merge-20260906/integration-r3/output.log`。

## 2026-09-06 — Owner wheel closure

R2 的临时环境来源断言仍失败，说明共享第三方 site 的环境不能依赖 pip 默认同版本判定。安装使用 `--ignore-installed` 强制这组本地产物进入临时 venv，并保留具体越界模块路径诊断；不卸载或替换主机包。

全量 Python 首次缺 `conversation.py`；补 SDK wheel 所有权后 wheel-closure R1 进入已声明 cryptography 依赖缺失边界。安装测试原来 `--no-deps` 且空 venv；改为离线复用测试主机第三方依赖，同时强制每个已导入 DI 模块来自临时 venv，保留 wheel 文件不碰撞与卸载后不可导入的断言。原始 `.codex-tmp/merge-20260906/wheel-closure-r1/output.log`。

## 2026-09-06 — Full Python diagnostic R2

后续 current-fixes R1（7 failed / 123 passed）与 R2（5 failed / 53 passed）首边界已定位为旧 fixture 和本机新导出模型与远端固定 registry 的身份差异；显式本地临时 registry 保留严格 hash 校验，修复依据见 [resolution design](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

3079 passed / 128 failed / 39 skipped，exit 1；旧实验目录/冻结基线缺失、显式 native binary/model 输入未设置，以及当前 facade、clock、build-prefix 测试夹具漂移。按既有 `run_spec175_python_gate.py` 的历史诊断与当前兼容性门分工，保留全量失败，不改旧 hash，不重跑历史 SIF/Tiger；当前合并覆盖的 Core/Repo/UAV/DI 测试另行明确选择并修复。原始 `.codex-tmp/merge-20260906/python-r2/output.log`。

## 2026-09-06 — Provider detached fetch lifetime / integration R2

GDB 捕获旧 Provider assignment worker 在销毁后调用 `Face::getIoContext()`；不是当前 targeted-stream 用例的独立失败。改为 Provider 所有的有界 fetch pool，关闭时取消等待、join，排队回调先检查共享关闭标志。原始 `.codex-tmp/merge-20260906/integration-r2/output.log`，完整记录见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

## 2026-09-06 — Python collection and generation fixture R2

Python collection 缺 Repo binding 和三个既有辅助源脚本；integration 多 Provider generation fixture 的新 input endpoint digest 与旧常量冲突。分别补构建闭合/输入脚本和独立 endpoint identity，保留首边界证据。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

## 2026-09-06 — Real-NFD readiness collector R1

实际两轮启动/独立 Face round trip 已发生；旧 collector 要求 challenge Data 名后还有 `/`，与新 exact reply 不符，exit 1。修正 exact token 匹配并重跑，不以 collector failure 推断协议结果。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Controller readiness / versioned NAC parameters

Readiness R1 在 PUBPARAMS 首边界超时，exit 124。旧随机后缀探针与新 NAC 固定 generation 名称不兼容；分离 Authority fresh challenge 和当前版本参数验证，不回退 NAC 的版本真实性约束。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

## 2026-09-06 — Integration fixture build R4

D2a fixture 修订时保留了该 case 未定义的 `selectionObserved` 标志，编译拒绝；删除无关赋值后 R5 重建。Unit R2 已 751/751 PASS，不能替代 integration。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Native executable link and full integration R1

Native target 缺 RuntimeStatusStore source；全量 integration R1 60/92 PASS 后以 139 退出，包含 targeted stream memory fault 和旧 wrapper 导入。先补 target source、重建同源 binding 并用 GDB 定位崩溃。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Merged legacy ingress integration R1

完整 suite 中旧 ingress 流程报告空 assignment、错误 role 和 D2b 未完成；保留原始 R1 后定向检查 Selection 首边界。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Merged full unit R1

747/751 PASS，4 failed（2 aborted），exit 201。空 tensor overflow 检查除零、manifest size 和选中 Provider 加密输入流程失败；逐项定位，不把旧失败当作允许项。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Merged SVS catch-up arguments

NDNSF build R1 捕获混合调用：catch-up 方法名已恢复，但自动合并保留旧方法的 bool 参数尾部。恢复调用方已有的数量和毫秒年龄实参；保持现有接收与权限语义。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#resolution-design)。

## 2026-09-06 — NAC installed pkg-config paths

首次 CMake 配置的 `.pc` 在 GNUInstallDirs 之前生成，include/lib 错指 prefix 根。显式 Waf prefix 不足以保证 Python 绑定使用新依赖；调整 NAC 初始化顺序并验证 fresh-config 导出。运行代码未变，完整 case 结果保留。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md)。

## 2026-09-06 — NAC AttributeAuthority test identity isolation

依赖 full test 41/46 PASS；五项 fixture 默认 Face 读入主机旧 PIB/TPM，尚未测试授权行为即签名失败。显式传入已有内存 KeyChain，保留断言并重跑完整 suite。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#validation-plan)。

## 2026-09-06 — Merged Context Mode fixture contract

合并 guard 后 42 PASS / 3 FAIL，首次边界为旧 Claude fixture 的 platform/registry 配置；更新有效 fixture，保留缺 hook/缺 flag 的失败断言。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md)。

## 2026-09-06 — Integration dependency preflight R2

Boost 路径修复后，NAC test link 仍调用 Linuxbrew ld，系统 OpenSSL/dl 符号解析失败。固定系统工具链再构建；原始 R2 日志和首边界见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#build-preflight-r2)。

## 2026-09-06 — Integration dependency preflight R1

NAC-ABE test link 选中缺失的 local Boost 1.82 库；NDNSF configure 拒绝尚未安装的目标 NAC prefix。均为构建前置失败，不是协议结果。先固定系统 Boost 并完成依赖测试安装，再重跑 NDNSF configure。见 [integration evidence](../specs/182-native-di-python-bindings/evidence/integration-20260906.md#build-preflight-r1)。

This is the repository-level index for failed, blocked, and `UNQUALIFIED`
attempts. It is an engineering memory, not a replacement for the active Spec,
the source tree, or a raw run directory.

## Read-first rule

At the start of every substantial task, read the newest entry in this file and
open its durable evidence record. If the entry names a raw log under the
ignored workspace temporary directory, inspect that log with targeted
`rg`/`tail` queries before choosing the next command. Then read the applicable
documents in
[`architecture-reading-guide.md`](architecture-reading-guide.md).

A failed preflight, startup barrier, or evidence collector is not a protocol
result. Do not retry a later gate, reuse a candidate, or claim a PASS until the
failure's controlling boundary and invalidation effect are understood.

## Current failure index

**Experimental consolidation closure (2026-09-06): development checks PASS.**
原生合并 `c770f18b` 的unit759/759、integration154/154、current Python
2171 passed/22 skipped和三个MiniNDN场景均PASS；新目录关联工具162 passed/
3 skipped，新Tiger工具58/58 PASS。下方collector RED由精确证据核对和进程组
清理修复关闭；原始失败保留。Tiger Local R8和B003运行验收仍未关闭，用户已暂停实验。
见 [integration closure](../specs/182-native-di-python-bindings/evidence/integration-20260906.md)
及 [Tiger baseline](../Experiments/TigerCluster/docs/two-node-baseline.md)。

**Tiger baseline collector review R1 (2026-09-06): expected regression RED.**
新增ACK/provider/request/selection与wrong-root边界证据负例在旧collector上
19 failed / 13 passed / 20 deselected，0.23s，证明它会接受不完整或矛盾证据。
这不是网络结果；保留 `.codex-tmp/merge-20260906/tiger-baseline-collector-red-r1/`。
修复后相关unit必须通过，B003实际运行仍未完成；见
[baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md)。

**Tiger two-node baseline Local R8 (2026-09-06): wrong-root boundary mismatch.**
All normal service, permission-rejection and cleanup checks passed. The wrong-root
child rejected PUBPARAMS authentication with abort134 before permission delivery.
Preserve this FAIL; classify only this exact isolated authentication abort in the
next run, rejecting unrelated crashes/timeouts. See the
[baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R7 (2026-09-06): observer/lifecycle FAIL.**
The service returned correct ECHO, but generic V2 binding leaves authentication
metadata unset. Replaced the unavailable-field assertion with observed native
ACK/Selection plus an actual wrong-root rejection obligation. The old Controller
wrapper cannot join its infinite native loop; use the existing C++ executable.
See the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R6 (2026-09-06): permission bootstrap FAIL.**
Raw signed roundtrips, signature negatives and PUBPARAMS succeeded. Controller
lacked public target certificates in its PIB and refused permission encryption;
added public-only imports with ndn-cxx readback and unchanged private-key checks.
Also isolated session state and corrected TERM ordering for FUSE-backed containers.
See the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R5 (2026-09-06): pre-NFD import FAIL.**
The image exposes UnixFace in stream_socket, not stream_face. Runtime preflight
stopped both workers before NFD startup; corrected the actual module path.
See the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R4 (2026-09-06): probe import FAIL.**
Multiline NFD/route configuration succeeded on both local instances. The probe
used KeychainSqlite instead of the image's KeychainSqlite3; corrected the symbol
and moved full application imports into pre-NFD inspection. See the
[baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R3 (2026-09-06): NFD management startup FAIL.**
Both NFDs aborted during internal FIB registration (10021). Review found compact
INFO list entries could not preserve privilege/policy nodes; restored multiline
INFO generation before retry. No protocol result; owned processes reaped. See
the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R2 (2026-09-06): identity setup FAIL.**
ndnsec refused root certificate installation into a nonexistent role-local root
identity. The validator already loads the public root file; removed the redundant
PIB installation. No NFD started; private state was cleaned. See the
[baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger two-node baseline Local R1 (2026-09-06): host preflight FAIL.**
The local Python lacks str.removeprefix; version parsing stopped before identity
or NFD startup. Replaced it with an explicit prefix check and slice. Raw output
and subsequent attempts are indexed in the [baseline record](../Experiments/TigerCluster/docs/two-node-baseline.md).

**Tiger review sync R1 (2026-09-06): 58/58 tool checks PASS.**
Adopted the reviewed supervisor fixture expectation for the existing
collector-before-terminal-validation order, retaining FAILED/cleanup checks,
and restored sys.path after profile-test import. The 51 prior checks plus
7 collector positive/negative checks pass; the migration R1 assertion failure
is closed. No production runtime or cluster was run. See [review sync evidence](../specs/182-native-di-python-bindings/evidence/tiger-directory-migration-20260906.md#review-sync-r1).

**Tiger directory migration unit R1 (2026-09-06): 50 PASS / 1 FAIL.**
The supervisor no-result unit expects TERMINAL_RESULT_MISSING but receives
CollectionError from the collector that now runs first. All 64 moved files
retain their original bytes and modes. The same named test with identical
bytes in a separate physical pre-migration layout reproduces the same failure;
this remains a baseline tool/test issue, not a relocation regression.
No SIF or Tiger execution occurred. See [migration evidence](../specs/182-native-di-python-bindings/evidence/tiger-directory-migration-20260906.md).

**Spec181 delivery-tool counterfactual R1 (2026-09-06): expected semantic RED.**
The 40-check baseline passes. Removing only the exit-code rejection makes the
same named regression fail with DID NOT RAISE, proving it detects false PASS
despite a nonzero child exit. Preserve the mutant and restore the production
check before final validation. Real T008 qualification and sealing remain open.
See [delivery tool evidence](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t009-delivery-tool-20260906.md).

**Spec181 T008 exporter adoption R1 (2026-09-06): focused PASS.**
The isolated exporter/adapter/numerical run passes 41 checks with no skips.
Explicit checkpoint input, actual 32/640 ONNX export, registered signatures,
640 CPU ORT versus PyTorch oracle, and production User negative branches are
covered. Preserve 22 fixed-shape export warnings; candidate local-delivery
tool closure and complete qualification remain open. See
[exporter adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#exporter-adoption-r1).

**Spec181 T008 input closure R3 (2026-09-06): focused PASS.**
The isolated inventory/supervisor run passes 92 checks after the actual RED.
Checkpoint files and registry-referenced public keys are bound; changed inputs
are rejected before children. The shared wrapper output-source dependency is
included with explicit CLI precedence and missing-input rejection. Three
registered Ed25519 public keys pass digest/identity checks. Full qualification
remains open. See [R3 closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#input-identity-closure-r3-and-public-material).

**Spec181 T008 input identity R2 (2026-09-06): missing shared CLI dependency.**
All new input checks pass; the isolated two-file run has 89 PASS and one FAIL.
The existing streamed-generation wrapper regression exposes an unadopted
runner-owned output-directory option. Preserve its CLI failure, adopt only
the output-source/default rejection behavior, and retain the existing test.
See [R2 boundary](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#input-identity-closure-r2-and-shared-wrapper-dependency).

**Spec181 T008 input identity R1 (2026-09-06): semantic RED.**
Fourteen focused checks expose unbound checkpoint and registry public-key
files; five existing drift checks pass. The actual gate attempts its child
entry after either new input changes, caught before a real child launch.
Repair the inventory input owner; no full suite or network case ran. See
[input identity R1](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#input-identity-closure-r1).

**Spec181 T008 assembly closure R4/R1 (2026-09-06): focused PASS.**
The corrected shared-source registration builds both complete C++ test targets.
Only the three named assembly cases ran: 3/3 cases and 138/138 assertions pass
through actual CPU ORT load/warmup with bound Provider/model/plan identities.
The complete suite remains blocked on remaining source/input closure. See
[assembly closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#assembly-test-build-and-focused-closure).

**Spec181 T008 full test build R3 (2026-09-06): source-registration link failure.**
The migrated assembly test compiles, but its shared preparation implementation
was added to grant_sources instead of di_integration_sources. Move that
registration; no suite ran. See [assembly migration](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#assembly-test-api-migration-plan).

**Spec181 T008 full test build R2 (2026-09-06): stale assembly-test API.**
The ProtectedRuntime migration compiles; integration compilation now stops at
ndnsf-di-native-assembly.t.cpp:341, which calls unavailable runtimeMetricsSnapshot().
No complete suite ran. Preserve real ORT load/execution proof while migrating
the check to the current runner contract. See [full build R2](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#full-test-build-r2).

**Spec181 T008 adoption C R1 (2026-09-06): unadopted GPU-source dependency.**
Fifteen checks stop at fixture compilation on the absent CudaDeviceIdentity header; one
source assertion exposes the old GPU metadata path. Inspection confirms this
draft targets uncommitted CUDA/profile changes. Preserve it for T010/T011
preparation, retain CPU-only local claims, and carry the concrete GPU evidence
gap into delivery. No production source was changed. See [Batch C](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-c).

**Spec181 T008 test adoption B R2 (2026-09-06): focused PASS.**
All 19 application/Merge checks pass without skips using explicit signed
canonical inputs. The stale timeout assertion follows the existing request
budget contract; no production deadline changed. Five local test/tool
dependencies remain. See [Batch B](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-b).

**Spec181 T008 test adoption B R1 (2026-09-06): stale source assertion.**
Eighteen checks pass; one expects a historical hard-coded User no-progress
timeout. Inspect its current parameter source before migrating the assertion.
No network attempt occurred. See [Batch B](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-b).

**Spec181 T008 runtime test migration R2 (2026-09-06): focused PASS.**
The final two-file target builds and passes 30 cases / 225 assertions.
Real verified grants now cover publish/fetch rejection and host/device lease
cleanup; credential-free checks remain fail-closed. Full-suite build and
source closure remain open. See [runtime test migration](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#full-test-build-r1-and-runtime-test-migration).

**Spec181 T008 full test build R1 (2026-09-06): stale test API.**
Compilation stops at the old ProtectedRuntime revoke/revoked calls; no full
suite executed. Keep revocation transferred, migrate current fail-closed
tests, and retain dataflow/zeroization assertions using real BoundGrantFixture.
See [runtime test migration](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#full-test-build-r1-and-runtime-test-migration).

**Spec181 T008 test adoption R3 (2026-09-06): focused PASS.**
The final four-file projection passes all 32 checks without skips after
removing the rejected temporary-path fallback. Source/test bytes match the
isolated projection; the complete C++ test-target build continues separately.
See [test adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-a).

**Spec181 T008 adoption checkpoint (2026-09-06): commit hook blocked.**
The staged role-assembly test retained a development-temporary-path fallback.
No commit was created. Remove that fallback, keep explicit input validation,
and rerun the focused checks before committing. See [test adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-a).

**Spec181 T008 test adoption R2 (2026-09-06): focused PASS.**
All 32 checks pass without skips using explicit canonical-package/registry
inputs. The four adopted files cover ACK provenance, truthful negative
verdicts, certified role assembly, and input/terminal ownership. Fifteen
other draft-test dependencies remain to review before complete qualification.
See [test adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-a).

**Spec181 T008 test adoption R1 (2026-09-06): 31 PASS, one input failure.**
The role-assembly regression's hard-coded isolated registry lacks its
catalogue-authority public-key file. Failure precedes assembly at signature
preflight. Bind the test to the explicit package and registry already used by
R19, retaining signature verification. See [test adoption](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#test-adoption-batch-a).

**Spec181 T008 case-configuration R2 (2026-09-06): focused PASS.**
All 76 inventory/supervisor checks pass, including actual per-case child
configuration and pre-launch input-drift rejection. The complete T008 gate
still needs test-source/build closure and final case inputs; R19 remains the
completed T005 subject. See [R2 review](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#focused-configuration-r2-and-review).

**Spec181 T008 case-configuration R1 (2026-09-06): focused RED.**
Eleven assertions expose missing declaration validation, unbound case-policy
bytes, and absent per-case child configuration; four existing input-drift
checks pass. The repair stays in the local inventory/supervisor boundary.
See [configuration preflight](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md#focused-configuration-r1).

**Spec181 formal Y-N R19 (2026-09-06): PASS; T008 preflight remains open.**
The maintained CLI completes all seven subcases, including three real grant
variants, on ce6a4ba0. All 63 application child exits are collected; source and
input identities remain unchanged, and no recorded PID or NFD remains.
See [R19 qualification](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-y-n-matrix-current.md#current-r19-qualification).
The next complete local gate needs case-specific configuration binding and
test-source closure; see [T008 preflight](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t008-local-suite-preflight-20260906.md).

**Spec181 exact-wire native R1 (2026-09-06): PASS.**
Committed ce6a4ba0 passes maintained native build and independent verify.
Both focused regressions also pass against the refreshed Core (21 and 270
assertions). The affected 12-dimension convergence review restores A05 PASS;
proceed to a fresh R19 matrix, retaining all earlier failures. See
[native review](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#native-identity-r1-and-convergence-review).

**Spec181 exact-tensor R6 (2026-09-06): focused PASS.**
Nine real DI/Provider/IMS cases pass 270 assertions: 1.4 MB compact transfer
fits signed packets (maximum 8,477 bytes), legacy format reconstructs, and
signature/commitment/context/bounds plus authenticated inner HMAC/index/legacy
binding mutations reject at their named boundaries. Commit the shared repair,
refresh full native identity and re-audit before a new formal matrix. See
[wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-repair-r5-and-authenticated-inner-rejections-r6).

**Spec181 exact-tensor R4 (2026-09-06): legacy fixture exceeds packet limit.**
Large compact transfer and four real verifier rejections pass (5/6 cases).
The legacy fixture's 7,000-byte payload segment plus old metadata reaches
10,555 signed bytes; Core correctly rejects before decoding. Use small legacy
segments to test compatibility, retaining the unchanged 1.4 MB compact case.
See [wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-repair-r3-and-negative-probe-r4).

**Spec181 exact-tensor R2 (2026-09-06): test bridge correction required.**
Compact production transfer reconstructs the 1,400,017-byte object and all
signed packets fit; 412/413 assertions pass. The sole failure counts 404
packets versus 202 because both fixture peer bridges and manual bridges run.
Disconnect fixture peer bridges before custom forwarding; keep the same
packet-count, content and size assertions. See
[wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-probe-r2).

**Spec181 exact-tensor R1 (2026-09-06): semantic RED at publication.**
The real DI publishOutput of a 1,400,017-byte tensor creates a 17,546-byte
signed manifest and correctly fails the repaired Core limit. Build passed;
the failure is the remaining codec boundary. Apply compact exact encoding
with authenticated reconstruction and legacy compatibility before retry.
See [wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#tensor-regression-r1).

**Spec181 exact-wire Core R2 (2026-09-06): focused PASS.**
Initial inventory rendering separately rejected the new evidence file's missing
layer header; add the explicit scoped header before rerunning that document check.
The production Core rebuild and unchanged Provider/IMS regression pass all
21 assertions: 8,799/8,800-byte signed packets are readable; 8,801-byte packets
reject and a late batch size failure exposes no earlier item. DI compact
representation/consumer repair remains BLOCK before native refresh and matrix.
See [wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#core-repair-r2).

**Spec181 exact-wire Core R1 (2026-09-06): semantic RED reproduced.**
The real Provider/IMS pull regression passes 18/21 assertions but accepts an
8,801-byte signed Data and leaves an earlier batch item readable after a late
oversize item. Adopt full signed-wire prevalidation for the whole batch, then
repeat the unchanged test in a fresh run. See
[wire repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-exact-data-wire-repair-20260906.md#core-regression-r1).

**Spec181 R18 diagnosis (2026-09-06): BLOCK at NDN Data wire size.**
The first actual send boundary is now identified: BackboneNeck's exact
MANIFEST Data encodes to 19,658 / 10,883 bytes and a SEG Data to 14,191,
above ndn-cxx's 8,800-byte limit (169 event-loop exceptions). The exact
NDNSF-DI filter exists; downstream deadlines are consequences. Review the
existing compact transport changes and pre-publication size guard as a
bounded shared unit, retaining signature/content commitments. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#exact-dependency-boundary-r18).

**Spec181 T005 formal R18 (2026-09-06): BLOCK at exact dependency transfer.**
BackboneNeck now executes with actual CPU ONNX evidence and completes its
role. DetectShard0/1 cannot fetch its exact tensor manifests; Merge then
times out on their outputs. Inspect publication, cache response and routing
before another run. Source/input identities remain unchanged and NFDs exit.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#exact-dependency-boundary-r18).

**Spec181 backend native refresh R1 (2026-09-06): PASS.**
Committed 214df1d6 completes maintained native build and independent verify;
both report native identity OK. The tested registration change and retained
device/error contracts pass affected convergence review. Resume a new formal
matrix; focused CPU checks do not establish matrix qualification. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-native-identity-r1).

**Spec181 backend repair R4 (2026-09-06): focused PASS.**
The maintained native Provider rebuilds (35.522 s); five real executable
checks pass (1.02 s). Legacy/public CPU names load and warm a real ONNX
model; unknown names and invalid execution-provider metadata still reject.
Commit only registration blocks and tests, then refresh native identity and
re-audit before the next matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-repair-r4).

**Spec181 backend probe R3 (2026-09-06): semantic RED reproduced.**
With corrected evidence assertions, legacy CPU load/warmup and unknown-backend
rejection pass; three public-name checks fail at missing registration. Adopt
only the two registration blocks, rebuild the maintained Provider and repeat
the real executable checks. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-regression-r3).

**Spec181 backend probe R2 (2026-09-06): registration RED and assertion correction.**
Three checks reach missing public backend registration. The legacy backend
actually loads/warms the model, but its test misreads existing string-valued
evidence and nested device fields. Unknown-backend rejection passes. Correct
the schema assertion before the next focused run. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-probe-r2).

**Spec181 backend probe R1 (2026-09-06): BLOCK at test collection.**
An extra parenthesis in the new test prevents collection (0.36 s); no native
process ran. Correct the test syntax and retain R1 before a fresh R2 probe.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#backend-registration-probe-r1).

**Spec181 T005 formal R17 (2026-09-06): BLOCK at backend registration.**
All four Providers pass external assignment validation and enter their handler.
BackboneNeck then fails with no NativeModelRunner backend registered:
onnxruntime-cpu; dependent-role fetch deadlines follow. Preserve that first
boundary, repair actual backend registration and re-audit before retry. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#native-backend-boundary-r17).

**Spec181 digest native rebuild R1 (2026-09-06): PASS.**
Committed e6f44b65 rebuilds the Core library, native Provider and Python
extension; maintained build and independent verify both report native identity
OK. Source/byte checks and exact assignment rejection remain intact. Affected
convergence review PASS; proceed to a fresh formal matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-native-rebuild-r1).

**Spec181 digest repair R3 (2026-09-06): focused PASS; native rebuild pending.**
Four actual C++ helper checks pass (1.73 s) after the isolated Provider emits
canonical lowercase hex. Exact assignment size/digest checks remain intact.
Commit this unit, rebuild the native runtime and re-audit before a new matrix.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-repair-r3).

**Spec181 digest probe R2 (2026-09-06): RED reproduced.**
All four actual-helper comparisons fail only at Provider uppercase hex;
User matches independent SHA-256. Normalize Provider output to the existing
canonical lowercase contract, preserving exact byte/size checks. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-regression-r2).

**Spec181 digest probe R1 (2026-09-06): BLOCK at linker selection.**
The focused helper probe fails at compilation/linking (4 setup errors,
1.60 s), before digest comparison: system g++ selects Homebrew ld from PATH.
Use the system toolchain explicitly and retain the original log. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-digest-probe-r1).

**Spec181 T005 diagnostic R16 (2026-09-06): BLOCK at external assignment validation.**
Core INFO logging shows each Provider receives and queues Selection, then
fails assignment preparation with external collaboration assignment size or
digest mismatch. The duplicate request log is not the controlling boundary.
Inspect assignment publication, fetch and validation on the same source.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#external-assignment-boundary-r16).

**Spec181 T005 formal R15 (2026-09-06): UNQUALIFIED after Selection commit.**
The sealed-plan repair reaches four-role Selection commit, then the User
receives REMOTE_RESPONSE_FAILED. Provider WARN logs contain duplicate request
rejections but no native execution failure reason; this does not yet identify
the controlling cause. Inspect the Core request/Selection boundary and obtain
bounded diagnostic logs before changing behavior. Source/input identities
stay unchanged and all NFDs exit. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#provider-response-boundary-r15).

**Spec181 sealed-plan repair R3 (2026-09-06): focused PASS.**
All 22 sealing/candidate checks (0.97 s) and 36 existing plan integration
checks (0.77 s) pass in the isolated source. Fetch references remain separate
from canonical identity, immutable and digest-bound; malformed values reject
before commit, while legacy/local-preparation defaults remain compatible.
Affected A05 review PASS; resume a new formal matrix after committing. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-repair-r3).

**Spec181 sealed-plan validation R2 (2026-09-06): BLOCK at reference validation.**
The projected field resolves sealing; five normal/legacy/coverage checks pass.
Five malformed reference checks fail because non-string false values and
control characters are accepted. Require strings and reject control chars,
retaining the deliberate empty local-preparation reference. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-validation-r2).

**Spec181 sealed-plan regression R1 (2026-09-06): RED reproduced.**
Ten focused checks reproduce the missing field at the actual V3 sealing
expression (1.72 s). Project the existing shared contract and validate
transport forwarding, digest binding, legacy defaults and invalid references.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-regression-r1).

**Spec181 T005 formal R14 (2026-09-06): BLOCK at sealed-plan reference closure.**
The shared assembly repair permits request planning to reach plan sealing.
The committed SealedCollaborationPlan lacks artifact_fetch_data_names,
already consumed by placement. Review/adopt the existing field and digest
binding, verify plan production/consumption, then re-audit. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sealed-plan-boundary-r14).

**Spec181 canonical binding/assembly repair R6 (2026-09-06): focused PASS.**
The isolated Waf parity target builds successfully; all 40 canonical,
candidate and actual C++/Python assembly checks pass (12.91 s). Actual YOLO
two-candidate binding, recipe and publication-port checks pass as well.
Affected convergence review PASS. Commit only the tested shared CPU unit,
then resume a fresh formal matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-and-assembly-repair-r6).

**Spec181 canonical assembly R5 (2026-09-06): BLOCK at focused harness inputs.**
Thirty-two Python checks pass. Eight native checks lack the required parity
binary; the extended recipe probe incorrectly includes the non-ONNX Merge
role in its graph assertion. Correct those focused harness inputs before
further validation; no network attempt was made. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-assembly-boundary-r5).

**Spec181 canonical recipe R4 (2026-09-06): BLOCK after binding repair.**
R3 actual binding/publication and 24 focused checks pass. Extending the probe
through real role certification reveals that the committed recipe rejects
COMPONENT_SET zero intervals. Add the reviewed component/external-initializer
assembly changes to this source unit; retain unrelated CUDA provider selection
outside the unit. See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-repair-r3-and-recipe-boundary-r4).

**Spec181 canonical-binding regression R2 (2026-09-06): RED reproduced.**
With the complete Python path and private offline NDN environment, actual
YOLO describe reproduces the missing canonical_graph_digest TypeError.
Proceed with the reviewed shared dependency unit and focused checks. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-regression-r2).

**Spec181 canonical-binding regression R1 (2026-09-06): BLOCK at probe import.**
The isolated probe lacks the Repo Python path and stops at SDK import before
the binding constructor. Complete the explicit Python/private NDN environment
for the next focused run. The source/reference contract,
shared deployment consumer and their tests form the reviewed dependency unit.
Validate that unit plus actual two-candidate publication before re-auditing.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-regression-r1).

**Spec181 T005 formal R13 (2026-09-06): BLOCK at canonical artifact binding closure.**
The epoch repair permits the User to request a V3 task. YOLO describe passes
canonical_graph_digest to the committed CanonicalArtifactBinding, whose
dataclass lacks that field. The working tree contains the shared canonical
reference extension, while the isolated commit omits it. Review and validate
the complete binding/publication dependency before another run; all NFDs
exited and source/input identities stayed unchanged. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#canonical-binding-boundary-r13).

**Spec181 subcase-epoch repair R2 (2026-09-06): focused PASS.**
All 117 runner/matrix/grant seam checks pass (3.99 s). Child epoch now matches
the publication/Provider runtime inputs; the protected Y-B and all three
Y-N-E mutations retain their grant configuration, and the parent environment
is unchanged. Affected convergence review PASS; T005 needs a new formal run.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#subcase-epoch-repair-r2).

**Spec181 subcase-epoch regression R1 (2026-09-06): RED reproduced.**
Seven plaintext cases inherit the protected matrix epoch; four protected
profiles pass. The focused production runner capture stops before network
startup (7 failed / 4 passed / 88 deselected, 2.06 s). Bind child epoch to the
same runtime inputs used by publication and Provider process specifications.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#subcase-epoch-regression-r1).

**Spec181 T005 formal R12 (2026-09-06): BLOCK at subcase epoch environment.**
Committed fixture loading passes. Y-N-O User inherits the matrix's protected
epoch, although runtime publication and process specifications choose the
plaintext control epoch; its grant seam then raises SPEC181_REQUESTER_PRIVATE_KEY
KeyError. Scope the child epoch to the selected subcase and regression-test
both plaintext controls and protected Y-N-E before rerunning. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#subcase-epoch-boundary-r12).

**Spec181 fixed-input closure repair (2026-09-06): focused PASS.**
The existing PPM and provenance README are adopted without byte changes.
All 22 numerical regression checks pass (7.97 s); the actual canonical
package reference loader verifies the fixture digest and shapes. A05 is
re-audited PASS; verify the committed fixture in the isolated checkout
before resuming the formal matrix. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#fixture-closure-repair).

**Spec181 T005 formal R11 (2026-09-06): BLOCK at fixed fixture source closure.**
The repaired lock permits Controller publication, Repo, and four native
Providers to become ready. User startup fails because the isolated commit
lacks tests/fixtures/spec180/yolo26n/fixed-fixture.ppm. The existing untracked
162-byte fixture matches the manifest digest. Reopen A05, adopt the fixture
and its provenance, validate the real reference loader, then re-audit before
the next matrix. Source/input identities stayed unchanged; NFDs exited. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#fixed-input-source-closure-r11).

**Spec181 T005 formal R10 (2026-09-06): BLOCK at Controller publication initialization.**
The explicit shell environment repaired process spawning. NFD readiness and
Controller startup pass, but the co-located publication ServiceUser constructor
throws `Failed to acquire file lock`. The maintained matrix exits 2 at Y-N-O;
The focused syscall probe finds EACCES before flock: the UID-0 lock path is
owned by UID 1000, with no kernel lock or fuser occupant. Preserve this stale
file in R10 before letting the runtime recreate it; no Core change is needed.
no protocol result is established. Source/input identities remain unchanged,
and all NFD processes exited. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#controller-publication-boundary-r10).

**Spec181 T005 formal R9 (2026-09-06): BLOCK at explicit shell environment.**
The new diagnostic identifies KeyError in Mininet node.py:419: shell=True reads
os.environ['SHELL'], absent from the launch environment. Preserve the exact
frame chain; set SHELL=/bin/bash explicitly for R10. No Controller was launched,
all NFDs exited, and source/input identities stayed unchanged. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-application-boundary-r9).

**Spec181 spawn diagnostic repair R3 (2026-09-06): focused PASS.**
All 93 runner/matrix checks pass. Partial-start cleanup and first-failure stop
remain intact; the new exclusive diagnostic records type and frame locations
without exception text or locals. The actual R8 spawn cause still requires a
new run. See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#spawn-diagnostic-repair-r3).

**Spec181 spawn diagnostic R2 (2026-09-06): BLOCK at test import.**
Two-file checks produce 1 failed / 92 passed (1.55 s). The runtime writes its
new diagnostic; the new assertion lacks the json import. Preserve R2, add the
test import, and rerun. This is a test-fixture failure, not a runtime result.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#spawn-diagnostic-regression-r2).

**Spec181 spawn diagnostic regression R1 (2026-09-06): RED reproduced.**
The existing partial-start cleanup test now verifies a durable error boundary;
it fails because process-start-failure.json is absent (1 failed / 87 deselected,
0.90 s). Keep cleanup and failure verdicts intact; record only exception type
and frame locations, preserving first evidence without messages or locals.
See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#spawn-diagnostic-regression-r1).

**Spec181 T005 formal R8 (2026-09-06): BLOCK at application spawn diagnostics.**
NFD readiness/routing/keychains pass. Controller log creation is followed by an
immediate spawn failure; the matrix preserves only CONTROL_NOT_PROVEN and drops
the underlying traceback boundary. Preserve R8 and add exception type plus
file/function/line frames, without exception text or locals, before another
diagnostic run. NFD cleanup was verified. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-application-boundary-r8).

**Spec181 T005 formal R7 (2026-09-06): BLOCK at NFD readiness.**
All five NFD sockets exist, but node nfdc checks fail; no application child
started. The launcher inherited offline probe NDN_CLIENT_* overrides pointing
to unused.sock instead of node client.conf. Preserve startup diagnostics and
logs. R8 will isolate the parent via private HOME, remove the global overrides,
and retain explicit Python dependency paths. NFD/native Provider cleanup was
verified. See [formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-network-boundary-r7).

**Spec181 T005 formal R6 (2026-09-06): BLOCK at Mininet executable readiness.**
The explicit PATH omitted sbin and Mininet could not find ifconfig (exit 1).
Preserve R6. Verify required network tools and append the system sbin paths for
R7 while preserving Python/native resolution order and recording the new launch
environment. This is startup readiness, not a protocol result. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r6).

**Spec181 T005 formal R5 (2026-09-06): BLOCK at launch environment parsing.**
The temporary parser rejected the registered SPEC180_CASE_OUTPUT_DIR in the
Y-N environment. No case/state or runner was created. Preserve R5; accept that
specific field and override it with R6's unique output. The five-role Y-N input
uses the same model; the protected epoch remains explicit for Y-N-E. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r5).

**Spec181 T005 formal R4 (2026-09-06): WAITING_EXTERNAL_INPUT at case roles.**
Envelope ownership now passes. The Y-B baseline configuration lacks Y-N's
required FullModel capability, so maintained validation exits 78 before network.
Preserve R4; inspect and use the existing Y-N-specific inputs for a new R5.
The registered role-set requirement remains unchanged. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r4).

**Spec181 T005 formal R3 (2026-09-06): WAITING_EXTERNAL_INPUT at key ownership.**
The repaired sudo source gate passes on 88e7a458. Maintained input validation
rejects the developer-owned envelope key for root execution (exit 78), before
network. Preserve R3 and provision the same bytes as a 0600 root-owned file in
R4's private state, retaining the original key untouched. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r3).

**Spec181 sudo source repair R2 (2026-09-06): PASS; formal R3 next.**
The gate preserves SUDO_UID only for root plus the actual selected checkout
owner. Real sudo positive/negative tests and existing local gate regressions
pass: 51 checks (8.68 s), with Git overrides still stripped. A05 is re-audited
PASS; T005 remains incomplete and will use a new run directory. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sudo-source-repair-r2).

**Spec181 sudo source regression R1 (2026-09-06): RED reproduced.**
Real sudo Git checks yield 1 failed / 1 passed: the legitimate owner is rejected,
while the wrong UID remains rejected. The test also supplies hostile GIT_DIR
and GIT_INDEX_FILE overrides. Preserve red.log and repair only the matching
sudo-owner identity in the sanitized environment. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#sudo-source-regression-r1).

**Spec181 T005 formal R2 (2026-09-06): BLOCK at sanitized source Git.**
Outer Git now accepts the actual sudo user, but production _source_git drops
SUDO_UID again and fails before network. Reopen A05's sudo checkout boundary;
add a real sudo regression and retain the UID only when it matches the selected
checkout owner. All Git override variables remain stripped. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r2).

**Spec181 T005 formal R1 (2026-09-06): BLOCK at launcher Git ownership.**
The explicit environment dropped SUDO_UID; root Git rejects the user's checkout
before invoking the maintained runner. Preserve R1. Retain the actual sudo
caller UID in the explicit launch environment for R2; do not write global Git
exceptions or weaken the source gate. No network started. See
[formal matrix](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-formal-matrix-20260906.md#first-boundary-r1).

**Spec181 T007 convergence (2026-09-06): PASS; T005 next.**
A05's source/configuration/input/build/application boundaries now map to their
focused regressions and final committed checks. A01–A12 are closed within their
documented scopes. The 12-principle audit permits same-source local validation;
it is not qualification or development-delivery PASS. Historical failures below
remain preserved. See [audit](../specs/181-ndnsf-di-protected-grant-qualification/audit.md#a05-closure-matrix).

**Spec181 committed source R13 (2026-09-06): R12 resolved.**
Repo build intermediates are preserved outside the checkout. The strict source
guard passes for 6b9bb51c, and the real application/native preflight plus four
application imports pass on that commit without network. Overall T007 audit
remains the next gate. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#committed-source-and-runtime-r13).

**Spec181 committed source R12 (2026-09-06): BLOCK at build intermediates.**
The isolated checkout matches 6b9bb51c with a clean tracked/index tree. The
actual source gate rejects untracked Repo setup build/src/ArtifactManifest.o.
Preserve the terminal result and move the complete intermediate build directory
to R12 before retry, retaining runtime extension and strict source checks. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#committed-source-reconciliation-r12).

**Spec181 source closure R11 (2026-09-06): focused failures resolved.**
The same isolated selected source passes 45 existing checks and 12 new candidate
binding checks. Actual application/native preflight and four application imports
pass without network. R1–R10 remain below as historical first-boundary evidence.
Final committed-source reconciliation and T007 audit remain open. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#focused-closure-r11).

**Spec181 focused source closure R10 (2026-09-06): BLOCK at request contract.**
Actual application/native preflight passes without network. Six-file checks
yield 9 failed / 36 passed (1.38 s): missing DIRequestEnvelopeV2 input transport
fields and InferenceApplication task arguments. Preserve R10 and close the two
request endpoints before retry. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-and-focused-r10).

**Spec181 application preflight R9 (2026-09-06): BLOCK at verifier implementation.**
Export alone is insufficient: ProviderOfferTrustVerifier itself is absent from
committed SDK provider.py. Preserve R9 and validate the implementation and its
existing signature/ACK tests as part of source closure. No network ran. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r9).

**Spec181 application preflight R8 (2026-09-06): BLOCK at public verifier export.**
Actual publication/process_specs/native guard pass. The post-guard import probe
then finds user.py requires ProviderOfferTrustVerifier missing from SDK exports.
Preserve R8 and add the existing verifier export; no network ran. Native guard
success alone does not prove application import closure. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r8).

**Spec181 application preflight R7 (2026-09-06): BLOCK at local launch helper.**
Runtime publication passes. Actual process_specs calls legacy python_cmd with
repo/py_dir, which the committed helper lacks. Close the matching local helper
parameterization, retaining default compatibility. Preserve R7; no network ran.
See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r7).

**Spec181 application preflight R6 (2026-09-06): BLOCK at catalogue conversion.**
SDK/Provider imports pass. Runtime publication requires the uncommitted
PreSplitCatalogSnapshot.from_mapping contract. Preserve R6 and close the
matching validation/serialization dependency before retry; no native guard or
network ran. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r6).

**Spec181 application preflight R5 (2026-09-06): BLOCK at Repo reference contract.**
The selected ApplicationInput contract requires LargeDataReference, absent from
committed repo_reference.py. Existing Provider/client/facades already consume
this shared publication/reference owner. Preserve R5 and close that dependency;
no network ran. See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r5).

**Spec181 application preflight R4 (2026-09-06): BLOCK at shared input contract.**
Candidate construction now passes. SDK imports then fail because the committed
Provider requires MAX_INLINE_INPUT_BYTES absent from adapters.base; the
coordinator also requires InputTransportMode. Preserve R4 and close the shared
input contract/export dependency before retry. No native guard/network ran.
See [runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r4).

**Spec181 application preflight R3 (2026-09-06): BLOCK at shared candidate contract.**
Repo's same-source build and import pass. Production YOLO publication then
constructs SplitCandidate with selection_priority, absent from the committed
contract, and fails before native guard/network. Preserve R3; close the exact
shared contract dependency without mixing unrelated worktree changes. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r3).

**Spec181 application preflight R2 (2026-09-06): BLOCK at Repo Python extension.**
Adding four existing YOLO source files to the isolated checkout clears actual
catalogue verification. Policy generation then cannot import
py_repoclient._py_repoclient, which has not been built in that checkout.
No native guard/network started. Preserve R2; use Repo's maintained build
against the same source, not an unknown worktree binary. See
[runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r2).

**Spec181 application preflight R1 (2026-09-06): BLOCK at committed Python adapter import.**
The clean a51f87b3 native build passes and emits its new receipt. Actual runner
validate_inputs then fails to import build_yolo26n_adapter from adapters.yolo,
wrapped as CANONICAL_CATALOGUE_VERIFY_FAILED. No network or native guard was
started. Preserve R1 and repair the committed adapter package closure before
retry. See [local runtime refresh](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-runtime-refresh-20260906.md#application-preflight-r1).

**Spec181 local input identity R3 (2026-09-06): BLOCK at new fixture import.**
After R2 66 PASS, new input tests yield 3 failed / 69 passed (7.68 s): three
tests reference json without importing it, before the identity owner runs.
Real-child drift/collection checks pass. Preserve R3, repair the fixture import,
then rerun. See [local input identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-input-identity-20260906.md#focused-r2-and-fixture-failure-r3).
R4 fixes the missing import: 72 PASS (8.40 s). Actual fixture children preserve
completed results and reject input drift before network cases or at final
aggregation. Both CLI discovery paths consume the explicit environment.
Configured input identity unit CLOSED; actual runtime audit remains T007 work.

**Spec181 local input identity R1 (2026-09-06): BLOCK at external input bytes.**
Model/map/referenced-key replacement and package additions reach the forbidden
child boundary after inventory creation (4 failed); launch configuration binds
path strings only. No qualification child ran. Preserve R1 and bind the actual
external inputs in the shared inventory/gate owner. See
[local input identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-input-identity-20260906.md).

**Spec181 explicit config root R1 (2026-09-06): BLOCK at protected launch selection.**
The runner overwrites explicit NDNSF_SPEC180_CONFIG_ROOT with the HOME default.
Absolute/relative overrides and missing-key rejection fail (3 failed / 1 passed,
0.81 s); fixture stops before native preflight/network. Preserve R1 and honor
the explicit root before child HOME changes. See
[explicit configuration root](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-explicit-config-root-20260906.md).
R2 honors the explicit root, resolves it before child HOME/cwd changes, and
rejects a missing explicit key even when the default key exists: 24 focused
checks PASS (0.85 s). Configuration selection unit CLOSED; T007 remains open.

**Spec181 Waf tool identity R3 (2026-09-06): BLOCK at CLI fixture PATH mismatch.**
1 failed / 79 passed (1.81 s): an old CLI linkage test builds with fixture PATH
then verifies with ambient PATH. The new Waf identity check correctly rejects
that mismatch first. Align the CLI fixture environment and retain its original
wrong-Core rejection assertion. R3 log and patch preserved in
[Waf tool identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-waf-tool-identity-20260906.md#cli-fixture-boundary-r3).
R4 repairs the CLI fixture: 80 PASS (1.75 s). Actual Waf directory selection
matches the new owner in both current and isolated checkouts (80 source/resource
files each). Waf source/selection unit CLOSED; runtime/input closure remains
T007 work. Existing native receipts require a maintained rebuild for the new field.

**Spec181 Waf tool identity R2 (2026-09-06): BLOCK at fixture executable identity.**
57 failed / 18 passed (2.77 s): the existing fake interpreter lacks its
executable bit, so real PATH resolution rejects it before mocked build; one
environment assertion also predates child-only WAFDIR. Preserve R2 and repair
fixtures before retry, without relaxing production resolution.
See [Waf tool identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-waf-tool-identity-20260906.md#fixture-boundary-r2).

**Spec181 Waf tool identity R1 (2026-09-06): BLOCK at generated build-tool identity.**
Four waflib content/location mutations escape native verification; interpreter
drift is rejected only after the native probe. Focused fixture checks: 5 failed,
70 deselected (0.36 s), no real build/network/qualification process.
Preserve R1, then bind actual selected Waf implementation in the maintained
native identity owner. See [Waf tool identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-waf-tool-identity-20260906.md).

**Spec181 local configuration R1 (2026-09-06): BLOCK at launch identity.**
Six focused mutations of environment, reserved output variable, declared
digest and interpreter bytes reach the forbidden qualification-child boundary
(6 failed, 0.78 s). The runner already uses explicit environment; the missing
check binds its actual values to the declared digest. No qualification child
ran. Preserve R1 before repairing the shared launch-configuration owner.
See [local configuration identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-config-identity-20260906.md).
R2 binds actual launch inputs through one shared builder/gate owner (52 PASS).
R3 verifies real child environment consumption and post-execution identity
failure (61 PASS). R4 adds builder/gate CLI round-trip and rejects changed
configuration before output/children: 62 PASS (5.80 s). Launch configuration
unit CLOSED; T007 remains BLOCK at runtime/input-byte identity planes.

**Spec181 revision 7 (2026-09-06): BLOCK at local configuration identity.**
Committed-source validation is closed in 2628e3d2 (39 focused checks and
the configured checkout PASS). The remaining controlling work binds actual
local configuration, build/runtime dependencies and development delivery.
By owner decision, SIF/replay/Tiger move to the experiment machine and are
not local closure prerequisites. Git merge is deferred until development ends.
See [scope transfer](../specs/181-ndnsf-di-protected-grant-qualification/evidence/development-scope-transfer-20260906.md)
and [current tasks](../specs/181-ndnsf-di-protected-grant-qualification/tasks.md).

**Spec181 local gate identity R8 (2026-09-06): checkpoint hook rejection.**
The local commit hook rejects assistant-directory references in production
source validation. Remove those non-product exclusions and rerun the focused
checks; do not bypass the hook. No checkpoint was created by the failed commit.
R8 removes the exclusions: 39 focused checks PASS (4.46 s), and the actual
configured checkout passes SOURCE_CHECKOUT_OK. The hook remains enabled.
The retry checkpoint succeeds as 2628e3d2; this hook incident is closed.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md#checkpoint-gate-r8).

**Spec181 local gate identity R7 (2026-09-06): source unit CLOSED; configuration BLOCK.**
All 39 focused checks pass (3.44 s), and the configured 1ba99000 checkout
passes exact source validation. Bad preflight identity has no qualification
child/output side effects; mutation during fixture execution yields
UNQUALIFIED while preserving child/cleanup records. Effective configuration,
generated build-tool/runtime bytes and external import bindings remain open.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md#focused-green-r7).

**Spec181 local gate identity R6 (2026-09-06): BLOCK at Python compatibility.**
New symbolic-link checks use Path.is_relative_to, absent from the maintained
interpreter. Focused checks yield 23 failures / 16 passes; the subsequent
read-only checkout probe hits the same AttributeError before qualification.
Preserve R6; use relative_to with ValueError handling, then require focused
success before the next checkout probe.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md).

**Spec181 local gate identity R4 (2026-09-06): BLOCK at Git LFS representation.**
The configured 1ba99000 checkout is rejected because a committed 134-byte LFS
pointer represents a materialized 240376592-byte release archive. Preserve
R4 before adding exact pointer size/SHA-256 verification; do not classify this
as source tampering or a protocol failure. No qualification child ran.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md).
R5 closes exact LFS-byte verification and passes 33 focused checks (2.51 s).
The next configured-checkout rejection is generated Waf tool code; classify
only its exact generated layout under the separate build-tool identity plane,
whose byte binding remains an A05 obligation. Preserve both R5 logs.

**Spec181 local gate identity R1 (2026-09-06): BLOCK at source authority.**
The real local gate accepts a fixture root without a Git HEAD and an invented
40-character sourceRevision, then reports PASS for six fixture children.
All six exits/cleanup records are collected; no network qualification ran.
Validate actual checkout/source identity before any qualification child or
output directory is created, then retain focused regressions for rejection.
See [local gate identity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-local-gate-identity-20260906.md).
R2 adds real Git fixtures and nine identity mutations; all nine reach the
forbidden child boundary instead of being rejected (9 failed, 0.63 s).
No qualification child runs; preserve the RED log and source patch before
adding checkout, index, tracked-byte and untracked-code checks.
R3 passes all nine rejections and existing gate/inventory checks (23 PASS,
2.39 s). Actual configured-checkout and submodule boundaries remain under
focused review; configuration/candidate identity is not yet closed.

**Spec181 native plan closure R1 (2026-09-06): BLOCK at projection behavior.**
Twelve missing header lines close the maintained local native build, including
the extension import/identity check. Focused plan/merge tests then yield
27 PASS / 2 FAIL: COMPONENT_SET postprocessing is rejected, and two PIPELINE
tensors collide in runtime scope with mismatched producer/consumer names.
Preserve R1 before applying the exact parser/scope repair; no qualification
matrix or model run was started.
R2 closes the parser/scope unit: 29 cases / 133 assertions PASS, including
unchanged transport authorization groups. Refresh native identity from its
source checkpoint before advancing the remaining A05 candidate/config audit.
The 1ba99000 checkpoint subsequently passes the maintained native build,
including a fresh extension import and runtime identity receipt (R3).
See [native plan closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-native-plan-closure-20260906.md).

**Spec181 native source diagnostic R3 (2026-09-06): BLOCK at DI projection declaration.**
A checkout refusal left the first native retry on the prior HEAD plus an
explicit source patch; that run is invalid as clean-commit evidence. It
terminated with two compiler errors: NativeCanonicalOnnxAssembler reads
canonicalArtifactName absent from the committed NativeSelectionProjectionV3.
The nine tested files were then matched to 1df718c8 and checkout completed.
Inspect the declaration and assignment path before a fresh recorded retry.
See [framework source closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-framework-source-closure-20260906.md).

**Spec181 framework source closure R1 (2026-09-06): BLOCK at assignment metadata.**
The isolated six-file dependency closure builds the production framework
library. Its reused lifecycle/assignment checks yield 16 PASS / 1 FAIL:
ServiceProvider loses artifactDataName while projecting a structured
assignment set into CollaborationContext. Preserve the R1 source patch and
result, then close the exact Provider transfer before retrying.
R2 carries the root name through single/structured assignments and rejects
conflicting roots. The isolated target links and passes 26 cases / 204
assertions. This framework boundary is closed; full native closure remains.
See [framework source closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-framework-source-closure-20260906.md).

**Spec181 committed native build R4 (2026-09-05): BLOCK at C++ source closure.**
Clean 6c7a0b23 passes configuration and Waf graph creation, then ServiceUser.cpp
fails to compile: AckAuthenticationEvidence is missing, followed by missing
registration and publish-result declarations. The implementation depends on
uncommitted framework declarations/companions. Preserve R4 and close those
exact dependencies before the next clean build; no protocol result exists.
See [committed native closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-committed-native-build-20260905.md).

**Spec181 committed native build (2026-09-05): BLOCK at Waf graph creation.**
Detached d67de87a configures successfully, but tests/wscript references an
untracked native assembly integration source. find_node returns None and Waf
fails before C++ compilation. No protocol result exists; preserve the isolated
checkout and repair the committed source dependency before retrying.
One focused identity regression also fails: changing loaded tests/wscript
bytes with unchanged mtime does not invalidate the native receipt, because
that Waf control file is missing from source fingerprints.
R2 binds the missing control file (70 identity checks PASS) and builds the
integration target, but its seven named assembly cases yield 3 PASS / 4 FAIL.
All four fail at certified recipe_digest validation before ORT loading;
preserve R2 and compare fixture serialization with the production contract.
R3 corrects the old fixture's quoted integer dimensions, leaving production
digest validation intact: 7 cases / 160 assertions PASS. The missing test
source and identity repair are ready for a checkpoint; a fresh committed
checkout must still pass the maintained native build. T007 remains BLOCK.
See [committed native closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-committed-native-build-20260905.md).

**Spec181 A05 qualification inventory (2026-09-05): CLOSED within focused inventory repair.**
The inherited inventory requires Q-C/Q-W but discovers only Spec180 Python
tests, omitting Spec181 protected-grant regressions. Two tests using the real
inventory builder and pytest collection fail; no formal network run started.
R2 repairs those cases (15 PASS); an old count assertion is corrected, and R3
passes 16 checks. R4 then exposes a second boundary: two tests show rehashed
case-source substitution is accepted by the real builder and gate. These use
fixture children, not a network qualification. Preserve R4 before repair.
R5 rejects both substitutions (17 PASS); the old missing-oracle fixture also
changed its source path and is now correctly rejected earlier. R6 keeps the
registered path while withholding its oracle to preserve that check's scope.
R6 passes 18 checks. R7 then finds three unsafe entry IDs accepted by the
validator although the gate joins IDs into output directories; no escaped
write is attempted. Reject these IDs at the inventory boundary before R8.
R8 passes all 21 checks: active scope/collection, registered case source/args,
safe entry IDs, existing evidence/oracle controls and wrapper compatibility.
The remaining A05 native/candidate effective-configuration audit stays BLOCK.
See [qualification scope repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-qualification-scope-20260905.md).

**Spec181 T007 evidence inventory (2026-09-05): CLOSED within document inventory scope.**
The R003 record reports zero Spec181 evidence files, while the current tree
contains 37. Four active evidence records lack an explicit header layer, and
the audit body still describes already-closed T002/shared-runtime gaps. This
invalidates the old completeness claim, not the linked raw test results.
Four layer headers are repaired; the current inventory covers 145 entries,
including both audit roots and one explicit SELF row. Drift/structure checks
PASS, and all 105 Spec180 evidence-file hashes match the before-scan. Raw scan:
ignored workspace temporary directory `spec181-evidence-inventory-20260905-r1/`.
See [complete inventory](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-evidence-inventory-20260905.md).

**Spec181 shared generation worker (2026-09-05): CLOSED by focused repair.**
Two queued coordinator regressions entered the model after cancellation or
deadline. The existing pre-run cancellation check passed (1/3 cases PASS,
14/18 assertions PASS). Propagating the shared guard through the registered
runtime/worker and state staging repairs the boundary: rebuilt 48 cases /
366 assertions PASS. T007 remains BLOCK for its remaining audit obligations.
See [generation worker authority](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t007-generation-worker-20260905.md).

**Spec181 shared preparation control R1 (2026-09-05): CLOSED as startup error; R2 focused PASS.**
The isolated launcher omitted the empty output directory; `validate_inputs`
raised `OUTPUT_ROOT_MISSING` before MiniNDN startup. No protocol result exists.
The unified native build passed. R1 is preserved; fresh R2 passed the protected
P-256 control with four verified Providers and seven collected child exits.
See [shared preparation closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-shared-preparation-20260905.md).

**Spec181 T002 native handler observation (2026-09-05): CLOSED; early result INVALID.** A focused test
was started before the repair build completed and ran the previous binary.
R2/green.log is preserved. The original build completed (59.612s), then the rebuilt
binary passed 46 cases / 242 assertions in a new log. The early result is a
validation orchestration error, not a protocol result.
See [native handler closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-handler-closure-20260905.md).

**Spec181 T002 native prepared output binding (2026-09-05): CLOSED (focused repair).** The production
prepared-runner validator accepts a Merge output budget changed from the sealed
Selection's K=300 to K=1. R1 fails one of two assertions after a passing positive
control. Exact output shape/type binding now rejects the mutation; rebuilt focused
checks pass 46 cases / 242 assertions. Source closure remains pending; see
[native handler closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-handler-closure-20260905.md).

**Spec181 T002 native Merge contract (2026-09-05): CLOSED (focused repair).** Direct production-runner
checks pass the numerical controls but expose four missed rejections: unknown
postprocess identity, numeric suffix, trailing shape delimiter, and wrong output
dtype. R1 is preserved (3/6 cases, 22/26 assertions passed). Rebuilt repairs pass
6 cases / 26 assertions; related evidence/readiness checks total 10 cases / 69
assertions. Handler source closure remains open; see
[native Merge closure](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-merge-closure-20260905.md).

**Spec181 T002 native worker authority (2026-09-05): CLOSED (focused repair).** Four real-worker
regressions show that cancellation/expiry after preparation or during compute
still returns results and retains the registered plaintext lease. Grant
verification is valid initially; the missing boundary is worker consumption.
R1 is preserved. Request guards now fence preparation, compute, cached results,
events, publication, and return; rebuilt focused checks pass 51 cases / 280 assertions.
T002 source closure and unified production rebuild remain open; see [worker authority](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-worker-authority-20260905.md).

**Spec181 T001 request lifecycle (2026-09-05): CLOSED.** Twelve registered-handler
regressions show that cancellation or Selection deadline expiry before/during
grant fetch or after preparation still reaches model execution. Grant expiry
alone does not enforce request lifetime. Preserve the first red run before
repair. Four final regressions also expose a missing comparison between the
grant-reference and Selection policy snapshot. Both repairs pass 151 focused
regressions and six real Python process cases (24 exits collected). All red
runs are preserved. T001 acceptance is complete; T002/T007 remain open. See
[request lifecycle](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-request-lifecycle-20260905.md).

**Spec181 T001/T002 maintained process integration (2026-09-05): focused defects CLOSED.**
R1 native fixtures build, but NFD's own management FIB registration fails
because the test configuration omits management authorization. The requester
then receives connection refused; no grant verification occurred. Preserve
the r1 raw run. R2 fixes NFD but requester bootstrap requires a real Controller
for NAC public parameters. Separately, ten successful P-256 runtime calls leak
24560 bytes / 630 allocations under ASAN despite the Zeroized state. Both
boundaries are recorded before repair. R3 fixes the EC ownership leak: ten
P-256 calls pass ASAN. The real Controller becomes ready, but requester
publication readiness times out before any Provider result; preserve r3
before instrumenting that boundary. R4 NDN logs/stack locate the wait in
ServiceUser construction (NAC decryption key): the fixture needs its own
requester policy and certificate bootstrap. R5 passes five cases; Python's
wrong-recipient rejection is correct, but the test incorrectly requires a
Core wire prefix on an internal typed exception. Preserve r5 and correct
the scoped assertion without synthesizing a Core response. R6 passes all 11
checks (10 real-process cases plus ASAN), with all 40 child exits collected.
T001/T002 full acceptance and T007 remain open. See
[process integration](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-t002-process-integration-20260905.md).

**Spec181 T002 P-256 production path (2026-09-05): focused defects CLOSED.**
R1 proves the runner overwrites an explicitly configured recipient-key map
with the Ed25519 offer-key map. Four other checks stop in fixture key
generation because this cryptography installation requires an explicit backend;
they do not prove a production refusal. R2 fixes the fixture: three actual
entry regressions fail (requester loader, Python Provider loader, map override),
and two wrong-curve rejection checks pass. R3 repairs those entries: 130 checks
pass, but the positive P-256 case reaches the production envelope creator and
fails because its EC key generation also omits the required backend argument.
All three raw results are retained. R4 repairs the production key generation;
134 focused checks pass, including bounded private-file rejection. The unified
native rebuild and a fresh four-recipient P-256 Y-B control pass: four native
grant verifications, terminal numerical match, seven collected child exits,
and empty staging. Full T001/T002 acceptance remains open.
See [P-256 production path](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-p256-production-20260905.md).

**Spec181 T002 recipient credentials (2026-09-05): focused defect CLOSED.**
The production factory accepts only Ed25519 private keys although T002 and
the native verifier also support EC P-256 envelopes. The rebuilt credential
regression runs eight cases; only P-256 loading fails before grant acquisition
with `provider recipient private key is not Ed25519`. The r1 build and RED
logs are retained. R2 loads validated P-256 PEM through the production loader;
all eight focused checks pass, including wrong-curve and permission rejection.
P-256 network acceptance and full T002 closure remain open. See
[recipient credentials](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-recipient-credentials-20260905.md).

**Spec181 T002 helper lifecycle (2026-09-05): focused lifecycle defects CLOSED.**
The native assembler waits synchronously for its helper, ignores request and
operation deadlines during assembly, observes grant cancellation/expiry only
after slow work, and can activate helper output beyond the role envelope.
Six real-process regression failures are retained in
`spec181-t002-helper-20260905-r1/red.log` in the ignored workspace temporary
directory. R2 builds but 14 native checks stop at the loader: the old installed
framework lacks `streamCancelled`. R3 binds the test executable and its
environment to the current build library: 26 focused checks pass. A new
source-fetch cancellation regression then proves that the parent recreates
the erased plaintext directory. R4 serializes protected staging writes
with runtime cleanup; 27 focused checks pass, including the new race. The
new RED log is retained in r3. The final unified native rebuild and a fresh
protected Y-B control pass: four actual grant verifications, three ORT CPU
roles plus native Merge, verified terminal output, and empty staging after
all seven child exits are collected. Full T002 acceptance remains in progress. See
[T002 helper lifecycle](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-helper-lifecycle-20260905.md).

**Spec181 T003 assembly parity (2026-09-05): focused build defects CLOSED.**
The fixed-vector Python lane passes 8 cases; the native lane fails 8 checks
because its required current-source test executable is not built yet. This
is a test-input boundary, not an assembly or protocol rejection. R2 compile
also fails before execution because the manual command omitted the installed
NAC-ABE package's `NAC_ABE_CMAKE_BUILD` definition. R3 also lacks the maintained
framework include path; r4 replaces the manual command with a focused Waf
target using the existing dependency configuration. R4 compiles but exposes
the framework's NDNSD link dependency; r5 adds that configured dependency.
R5 build passes; the final 19 parity checks pass, including real ORT CPU
execution and unchanged negative-cache state. Fixed-vector regeneration is
byte-identical. T003 is complete at its focused scope; T007 remains BLOCK. Preserve
`spec181-t003-assembly-20260905-r1` and build the production-entry fixture
before another attempt. See
[T003 assembly parity](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t003-assembly-parity-20260905.md).

**Spec181 T005 evidence preservation (2026-09-05): focused defects CLOSED.**
Five focused checks expose the legacy driver's destructive attempt handling,
runtime-dependent entry, and continuation after matrix failures. No network
processes are started. The repair retires this unsafe automatic retry path
and makes the maintained matrix stop at its first failed subcase. Raw RED
output is retained under `spec181-t005-evidence-repair-20260905-r1` in the
ignored workspace temporary directory. R2 passes 118 focused checks after
retiring the legacy entry and stopping on the first matrix failure. This is
not formal matrix qualification; T005 remains NOT PROVEN. See
[T005 evidence repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t005-evidence-repair-20260905.md).

**Spec181 T006 positive control (2026-09-05): cold dependency timeout CLOSED.**
All four native grants verify, but r8 Merge's first tensor-manifest fetch
expires at the fixed 10 s no-progress bound while its producer finishes cold
model preparation. User then times out. The three actual grant negatives
pass. After binding the data wait to the configured request budget while
retaining the existing hard deadline and cancellation, r10 completes the
protected native control; a measured dependency wait is 13.97 s. The final
r11/r12/r13 negatives also pass with complete process collection. T006 is
complete at its focused scope; T007 remains BLOCK. See
[T006 production repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t006-production-repair-20260905.md).

**Spec181 T006 production rejection (2026-09-05): false-positive oracle CLOSED.**
Eight focused regressions fail: the configured requester seam publishes no
mutation, invalid mutation/epoch settings are admitted, and User-local
exceptions or markers can masquerade as Provider rejection. No selected
Provider network rejection is established by those old probes. See
[T006 production repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t006-production-repair-20260905.md).
After repair, 62 focused checks pass. The separate runner check retains one
obsolete expectation that a User-local Y-N-E probe should report PASS; that
test is corrected; the updated Python group has 150 PASS. A separate C++
harness link omitted the store source; the next command used a nonexistent
shortened filename. The verified source is NativeProtectedArtifactStore.cpp;
use a new directory for the corrected harness command. The unified native
build has independently completed successfully.
r4 C++ harness passes 22 cases. The r5 live entry stops before network
creation because Y-B inputs omit Y-N's FullModel role. Use the verified
five-role Y-N inputs with an explicit protected epoch for subsequent variants.

**Spec181 T004 lifecycle acceptance (2026-09-05): focused defects CLOSED.**
R1 failed before the waiter because the fixture had no running Controller
serving AA public parameters. R2 corrects that startup order and reaches the
real Provider waiter: an 80 ms wait returns false in about 6 us before run,
with SPEC181_PROVIDER_READINESS_PREMATURE_TERMINAL. The initial non-running
state was incorrectly terminal. R3 retains an OUTPUT_ROOT_MISSING preflight
failure. After the native fix/rebuild, r6 waits 83 ms and starts/stops the real
Provider; r4 reaches Controller readiness at 12.39 s; r5 cancels an active
Core probe in 2.3 ms without hot spinning. All three probes exit 0 after
process collection and network cleanup; six Core checks also pass. T004 is
complete at its focused acceptance scope; T007 remains BLOCK. See
[T004 lifecycle acceptance](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t004-lifecycle-acceptance-20260905.md).

**Spec181 T002/T004 native repair (2026-09-05): focused defects CLOSED; tasks OPEN.**
Retained failures identify group digest format, grant forwarding hint, model
basename, premature readiness, and debugger exit-code boundaries. Live-r7
uses ordinary process commands and the same rebuilt source: native protected
Y-B returns PASS/exit 0, with three ciphertext files and no ONNX plaintext or
staging remnants after cleanup. Production negatives, cancellation/resource
acceptance, source checkpoint closure and T007 remain open. See
[native launch repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-native-live-repair-20260905.md).

**Spec181 T002 production wiring (2026-09-05): build defect CLOSED; T002 OPEN.**
The first compile failed on the installed ndn-cxx forwarding-hint API. A
separate retained r2 build passes native/library/extension identity checks,
and 3 rebuilt-extension tests consume the 9 grant vectors. Real Provider
network and ORT lifecycle acceptance remains open. See
[T002 production repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-production-repair-20260905.md).

**Spec181 T002 native runtime repair (2026-09-05): focused defects CLOSED.**
18 native focused tests pass with real verification, managed content keys,
deadline/cancellation checks and retryable cleanup. Initial compile, linker
and consumption/deadline failures remain preserved. Factory, storage AEAD
and real network acceptance still keep T002 open.
See [T002 runtime repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t002-runtime-repair-20260905.md).

**Spec181 T001 registry repair (2026-09-05): focused defects CLOSED.**
100 focused tests plus 7 inherited grant tests pass for pinned registry
policy, private-key matching, distinct issuer/publication identities and
the final published-root allowlist. Initial RED and Python 3.8 compatibility
failures are retained. T001 network and lifecycle acceptance remains open.
See [T001 registry repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-registry-repair-20260905.md).

**Spec181 T001 Provider repair (2026-09-05): focused defects CLOSED.**
70 focused tests pass for authorization before preparation, in-memory keys,
on-disk AEAD loading, model/weights cleanup and registered-handler failures.
Registry-policy wiring and real network integration still keep T001 open.
See [T001 Provider repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-provider-repair-20260905.md).

**Spec181 T001 lifecycle repair (2026-09-05): focused defect CLOSED.** Five
RED failures are repaired; 19 focused tests pass for in-memory key leases,
duplicate protection, complete cleanup and private/symlink-safe files.
Provider integration remains open; this does not close T001.
See [T001 lifecycle repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/t001-lifecycle-repair-20260905.md).

**Active Spec181 audit (2026-09-05): BLOCK.** Native protected runtime wiring,
production-path negative validation and assembly parity remain unproven;
the active Context Mode plan link has been repaired. Latest retained Spec181 Y-B log
reports `CASE_RUNTIME_PROCESS_START_FAILED:control`, not a protocol result.
See [Spec181 audit repair](../specs/181-ndnsf-di-protected-grant-qualification/evidence/audit-repair-20260905.md).
Focused repair checks are allowed; full qualification requires a fresh audit PASS.

**Current controlling repair (2026-09-05):** Y-N negative verdicts accepted
unrelated exceptions as PASS. The first focused regression reproduced 12
failures; the repaired User/runner/application/build-guard set passes 183
focused tests. The unified build also exposed a stale legacy RUNPATH in
`build-system-j2`, despite its name. See
[negative verdict and build repair](../specs/180-ack-driven-cross-model-qualification/evidence/t011-negative-verdict-repair-20260905.md).
FR-008 still lacks the production authenticated, recipient-encrypted grant
path and operator-authorized issuer configuration; see
[protected-grant gap](../specs/180-ack-driven-cross-model-qualification/evidence/t008-protected-grant-gap-20260905.md).
Older PASS labels below must not be reused as safety evidence. The r42 startup
observation remains useful but does not close this semantic verdict defect or
the missing FR-008 protected execution path.

| ID | Observed | Scope | First failing boundary | Disposition | Durable record | Raw run data |
| --- | --- | --- | --- | --- | --- | --- |
| `SPEC180-Y-N-R42-EXTENSION-CWD` | 2026-09-05 | Spec180 current-source extension rebuild | `pythonWrapper/setup.py` was invoked from the repository root, so its relative C++ source path could not be found | `CLOSED as command-invocation error`; compiler exited 1 before producing an artifact | [`t011-y-n-live-current-20260905-r42-extension-build-cwd.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r42-extension-build-cwd.md) | no raw run; command output is preserved in the task log |
| `SPEC180-Y-N-R41-I-EXTENSION` | 2026-09-05 | Spec180 focused Y-N-I current-source check | Loaded Python extension/framework artifact identity before interpreting the live protocol result | `CLOSED by r42`; rebuilt artifacts contain the current source and r42 logged the marker sequence | [`t011-y-n-live-current-20260905-r41-extension-check.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r41-extension-check.md), closure [`t011-y-n-live-current-20260905-r42-i-only.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r42-i-only.md) | ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r41-i-only/` |
| `SPEC180-Y-N-R40-PATH` | 2026-09-05 | Spec180 diagnostic I-only | Temporary run path before the maintained helper completed | `CLOSED as command-path error`; the command used a mistyped directory and was interrupted with exit 130 | [`t011-y-n-live-current-20260905-r40-path-preflight.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r40-path-preflight.md) | preserved partial data under ignored workspace temporary `spec180-diagnostic-path-error-r40/` |
| `SPEC180-Y-N-R39-I` | 2026-09-05 | Spec180 local MiniNDN Y-N | Controller `PUBPARAMS` readiness before Y-N-I reached provider execution | `CLOSED by r42 for the focused I-only boundary`; r42 reached readiness and provider execution with the rebuilt current artifacts; the full matrix remains open | [`t011-y-n-live-current-20260905-r39.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r39.md), closure [`t011-y-n-live-current-20260905-r42-i-only.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r42-i-only.md) | ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r39/` |
| `SPEC180-Y-N-R37-P-PREFLIGHT` | 2026-09-05 | Spec180 diagnostic P-only | MiniNDN root/user-namespace preflight before NFD creation | `CLOSED by r38`; diagnostic command omitted `unshare -Urnm` and exited 1 | [`t011-y-n-live-current-20260905-r37-p-only-preflight.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r37-p-only-preflight.md), closure [`t011-y-n-live-current-20260905-r38-p-only.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r38-p-only.md) | ignored workspace temporary P-only attempt `spec180-yolo-y-n-current-20260905-r37-p-only/` |
| `SPEC180-Y-N-R36-P` | 2026-09-05 | Spec180 local MiniNDN Y-N | Controller `PUBPARAMS` readiness before Y-N-P reached the ACK-closed boundary | `UNQUALIFIED`; Y-N-O/C/R/I/E/L passed, Y-N-P was not proven | [`t011-y-n-live-current-20260905-r36.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r36.md) | ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r36/` and console log with the same run-id |
| `SPEC180-Y-N-R35-P-E` | 2026-09-05 | Spec180 local MiniNDN Y-N | Controller `PUBPARAMS` readiness before either negative case reached the ACK disposition path | `UNQUALIFIED`; Y-N-O/C/R/I/L passed, Y-N-P/E were not proven | [`t011-y-n-live-current-20260905-r35.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t011-y-n-live-current-20260905-r35.md) | ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r35/` and console log with the same run-id |

Historically, `r42` was recorded as closing its artifact-identity and focused
readiness occurrence. Its rebuilt framework library and Python extension were checked,
and the I log shows the Controller
issuing `registering prefix: /example/controller` and the six NDNSF filters,
followed by successful root and `PUBPARAMS` registration, the readiness probe,
and the expected Y-N-I execution markers. This is focused T011 evidence only;
the full Y-N matrix and T014 remain open. This does not qualify the subsequently
changed readiness implementation or negative oracle, nor prove the complete
dependency closure now checked by the unified build. The earlier r39 I
occurrence was closed only at that historical boundary; its bytes are preserved.

The earlier `r36` result likewise showed the Controller
issuing `registering prefix: /example/controller` and the six NDNSF filters,
but the first Face connection closes before the root registration completes;
the reconnect installs only the six NDNSF routes. There is no successful
Controller-prefix registration and no `PUBPARAMS` filter before the Python
readiness timeout. This is classified as a startup/transport boundary failure,
not as an ACK disposition failure. r35 remains relevant as the prior broader
P/E occurrence.

The ordinary `ConfigManager` message about a missing `/etc/ndn/ndnsf.conf` in
the child logs is ambient diagnostic noise for this run; it is not the
controlling failure because the successful subcases contain it as well.

## Historical pointers

These records remain useful when the current failure is related to their
boundary, but they do not advance the active gate by themselves:

- [`t013-controller-pubparams-readiness-current-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t013-controller-pubparams-readiness-current-20260904.md): why the real AA `PUBPARAMS` readiness barrier exists and why the old exact-SIF result was invalidated.
- [`audit-revision123-design-code-conformance-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/audit-revision123-design-code-conformance-20260904.md): revision-123 design/code findings and their evidence boundary.
- [`t014-tiger-path-audit-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t014-tiger-path-audit-20260904.md): Tiger-path audit block and the reasons implementation checks were not qualification evidence.
- [`t013-supervision-repair-20260904.md`](../specs/180-ack-driven-cross-model-qualification/evidence/t013-supervision-repair-20260904.md): supervision and cleanup caveats retained after the repair.

## Failure-record contract

Every failed, blocked, or `UNQUALIFIED` command that can affect task order
must produce or update a durable record before the next retry. The record must
contain:

1. a unique run/failure ID and UTC/local date;
2. the exact source/candidate/config identity and command;
3. the first failing boundary, exact marker or error, and child exit status;
4. the raw-log path plus a compact, secret-free excerpt or structured summary;
5. the affected Spec task/gate and any candidate/evidence invalidation;
6. the next allowed action and the condition that closes the failure.

Keep large logs, private keys, credentials, and transient sockets out of Git.
Use a new ignored workspace temporary directory named with a unique run-id for
each attempt; never overwrite a prior run. The durable evidence record must be
sufficient to understand the failure when the transient directory is later
unavailable.

## Closing an entry

Change the disposition only after a fresh run reaches the same boundary and
produces the required closing marker. A focused unit test can close an
implementation defect, but it cannot close a live, SIF, or Tiger gate unless
the active Spec explicitly defines that test as the gate's evidence.
# NDNSF Failure Log

## 2026-09-06 — UAV API compatibility audit: static authorization expires without renewal

- **Symptom**: an application with unchanged static permissions can compile with the new APIs but lose authorization at the Controller status's 24-hour boundary. Re-fetching the status does not extend its fixed validity window. Source probes also reproduce seven old-call compilation failures: deleted public large-response helper, three positional aggregates, two Hybrid member pointers and one NAC ParamFetcher member pointer.
- **Cause**: Controller initialization/epoch advance sets `m_policyValidUntilMs`; ordinary `getPolicyStatus`/Interest handling does not renew it, while new runtime enforcement rejects expired status. Separately, the helper removal, inserted fields and overload additions break specific source forms. Asynchronous DKEY bootstrap and ACK payload redaction introduce further behavioral migration requirements.
- **Disposition**: audit only, production fixes remain open. Do not present prior short MiniNDN scenarios as static long-running compatibility acceptance or restore insecure fallback behavior. Prioritize safe immutable-status renewal, production readiness semantics, then source compatibility/migration.
- **Evidence**: `specs/179-request-scoped-confidentiality/evidence/uav-api-compatibility-audit-20260906.md`; `results/api-compatibility-audit-20260906/summary.json` has 40 compilations (17 historical and 6 official controls pass; current 10 pass/7 compatibility failures). Six unmodified historical Apps/examples compile. A separately linked production expiry-boundary probe reports lifetime86400000, allowed-before1, allowed-at-expiry0, `controller_status_expired`, expired-refresh-accepted0. Its exit0 means defect reproduced, not fixed; no actual 24-hour network soak was executed.
- **Lesson**: preserve separate source, ABI, protocol, startup and long-running behavior gates. Applications that do not call new authorization APIs still enter the new runtime defaults.

## 2026-09-06 — API audit probe setup produced false compatibility failures

- **Symptom**: initial typed helper probe failed on both revisions (`std::string` lacks `ParseFromArray/SerializeToString`); NAC probes failed with unknown ParamFetcher/PublicParams. Trying a different include order alone did not solve the NAC failure.
- **Cause**: the synthetic payload did not satisfy the pre-existing template contract. Probe compiler paths allowed NAC algo headers' bare `common.hpp` to resolve to NDNSF's same-named header. These were invalid audit controls, not new library defects.
- **Fix**: use a minimal protobuf-shaped compile-only payload; put the NAC source/package directory ahead of NDNSF paths and use NAC's CMake header mode. Preserve initial logs under `results/api-compatibility-audit-20260906/setup-attempts/`, then rerun the entire matrix with at most two compiler processes. Final historical controls all pass, and the seven current failures are attributable to actual API changes.
- **Lesson**: never call a compiler red an API regression until the identical source compiles on the declared baseline with correctly isolated headers.

## 2026-09-05 — T022 official-merge segmentation regression setup

- **Symptom**: first new Producer segmentation test build fails because the default-template factories are protected. Two guessed inspection paths did not exist; the actual campaign is `scripts/spec179_minindn_campaign.sh` and Segmenter belongs to installed ndn-cxx. Initial T022 placement also made the structure scanner warn about task order.
- **Fix**: use explicit Data templates in tests without changing production access; locate exact files and move T022 after T021. Preserve the failed build log under `results/spec179-official-merge-20260905/nac-red-build.log`; retry at unchanged-j2.
- **Lesson**: test public behavior through explicit inputs; do not expose production helpers for convenience. Use file discovery instead of guessing script paths. This compiler failure is a test setup defect, not the expected pre-merge behavioral red.
- **Behavioral red**: fresh85547eb build plus four new cases gives12/15 pass,3 failed cases/5 failed assertions, exit201. CP/KP ignore128-byte limits (1500-byte data, single CK segment); normalizeCkKey collapses distinct segment-named objects and the second warm-cache decrypt returns wrong plaintext. Existing object/exact-segment retrieval and invalidation/reentry tests pass. Official58f3948 merge is conflict-free; green results pending.
- **Merged green**: Experimentalc3aafa6 retains85547eb and official58f3948 ancestry. The same15 cases all pass, including both segment-limit tests and distinct-CK plaintext. Separate prefix installed with f5cb1ec8 library hash; full NAC/native/network qualification continues.
- **Dependency gates**: full46/46,4284 assertions; installed-prefix26/26,1082 assertions. An initial launcher unittest command treated tests/minindn as an importable package and failed2 imports; rerun with PYTHONPATH=tests/minindn and explicit module names. Preserve the import-error log separately from actual launcher results.
- **Launcher runner correction**: that import-path retry executes0 unittest cases, so it is rejected as vacuous. These are pytest function tests; inspect their declared runner and execute python3 -m pytest on both exact files. Neither the import failure nor the zero-test exit0 is a launcher green gate.
- **T022 closure**: merged15/15, full NAC46/46 (4284 assertions), installed26/26 (1082), clean native183 unit/74 integration (11998/1297), pytest28/28; full18 MiniNDN scenarios,188 assertions and both dedicated User grant gates at clean cdd8e55a.33 hashes match disk, driver/all CLIs exit0. Build28m9.720s at-j2; no runtime source changes beyond the two official NAC files. Preserve the compiler/import/zero-case and behavioral reds. T014 publication is deferred, not failed or satisfied by the local merge.

## 2026-09-05 — NAC upstream provenance inferred from a remote alias

- **Symptom**: the first T014 delivery update called suraviregmi/NAC-ABE the original project and inferred12 missing prerequisites from its stale master.
- **Cause**: verified remote SHAs but did not first verify GitHub fork parent/source metadata. The alias `suravi` did not establish official ownership.
- **Correction**: GitHub API confirms both remotes fork UCLA-IRL/NAC-ABE. Fetching official master58f3948 into an isolated clone shows fork master2 ahead/3 behind and Experimental6 ahead/3 behind. The missing changes are maxSegmentSize propagation and removal of segment stripping, plus their merge. Corrected all current delivery/plan/task/audit claims; the preceding entry is historical and its original-project12-commit statement is superseded here.
- **Evidence**: both isolated no-commit merge previews pass without conflicts, changing only cache-producer.cpp and consumer.cpp; previews aborted afterward. Candidate trees and exact source diffs are recorded in `specs/179-request-scoped-confidentiality/evidence/nac-abe-official-comparison-20260905.md`. No candidate runtime qualification, actual NAC branch update, push or PR occurred. User explicitly requests no PR.
- **Lesson**: establish official repository identity from parent/source metadata, then fetch live refs and compare both ancestry directions. A clean merge proves textual compatibility only. A multi-file documentation patch with one mismatched context was rejected atomically; split it into verified exact-context updates without altering runtime files.

## 2026-09-05 — T014 upstream package described an incomplete, ambiguous delivery

- **Symptom**: the earlier package omitted NAC T019/T020, called personal fork master upstream, mixed ParamFetcher API changes into a no-API-change PR and treated the tested OpenABE worker as independently optional.
- **Cause**: delivery prose was not refreshed after dependency compatibility repairs or verified against actual remote/base ancestry.
- **Fix**: retain the old draft as explicitly superseded; pin all four commits through85547eb, map every public surface, document ABI/lifetime limits, provide a concrete fork PR draft, and distinguish the12 prerequisite commits between original master and the tested base. A standalone bundle passes verification, fresh clone, exact head/tree comparison and fsck. T014 stays open for publication authorization, upstream acceptance and rebuilt gates.
- **Tool deviations**: a query guard rejected low-entropy `T014`; corrected to the exact feature basename before authoritative reuse. Broad CodeGraph exploration returned unrelated version symbols; after the required attempt, exact source-string verification established callers. apply_patch rejected delete/add operations on one path atomically; a separate current report and historical pointer avoid destructive replacement. No runtime files changed.
- **Lesson**: an upstream package must identify the destination, prerequisites and complete tested revision; a local bundle or fork push does not prove upstream acceptance. Use a high-entropy feature identifier and update each path once per patch.

## 2026-09-05 — Provider online authorization was absent from MiniNDN grant coverage

- **Area**: Spec179 T021, Controller service-offering permissions.
- **Finding**: existing network grants hardcode User/B and `/PERMISSION`; Provider component policy assignment and network revocation do not establish first-grant service execution. Controller example's grant timer cannot select `/SERVICE`, and Provider example lacks the User example's explicit post-startup permission renewal.
- **Repair**: explicit User/Provider grant-role option (default User), Provider App-owned renewal timer, separate Provider normal/late first-grant scenarios with targeted traffic and unaffected control. Pure evaluators retain target/control failures and reject early service, wrong role/provider, absent renewal and missing late timeout ordering.
- **Evidence**: new evaluator before implementation fails11/11 because no evaluator exists (`provider-grant-evaluator-red.log`); after implementation11/11 pass. Combined launcher gate initially25/26 passes; sole failure is the old exact User-only guard-error string. Parameterize the host-namespace guard test for both roles; final27/27 pass (`provider-grant-launcher-final.log`). Native rebuild and network results pending under `results/spec179-nac-compatibility-20260905/`.
- **Lesson**: Controller policy mutation, runtime permission installation, key readiness and actual service execution require distinct evidence on each role. A User grant cannot qualify Provider service-offering authorization.
- **First network red**: normal and late Provider runs both complete but exit4/gatefalse (`provider-campaign-first/`, driver exit1); User compatibility control passes. Benchmark paths ignore `--known-provider-ids`, so Provider/A serves requests intended for B, including pre-grant successes. User compares provider/service records when deciding DKEY refresh, so an added Provider route incorrectly refreshes an unchanged User service attribute. Normal control31/32 and target31/32 also retain a transition timeout. Provider status-advance permission revalidation legitimately precedes the manual timer in the normal case, contradicting the initial test assumption.
- **Follow-up repair**: benchmark calls use the existing explicit-provider overloads, preserving built-in/custom selection; User DKEY change detection compares service sets, with an added route-only Controller integration regression. Evaluate actual post-grant Provider permission-fetch events and the later idempotent App timer separately. Use the existing grant probe's250ms status cadence; retain all terminal failures and document that version changes can cancel in-flight work, rather than claiming uninterrupted traffic at arbitrary timing. Rebuild/native/network verification pending.
- **Repaired native checkpoint**: build exit0 in4m27.010s at-j2; six dependency closures pass; launcher28/28, unit182/182 (11971 assertions), integration72/72 (1281 assertions) pass. Final18-scenario network acceptance pending.
- **Second network red**: at925ec3a9 both Provider variants pass every check except the exact selected Provider:17/17 and22/22 post-renewal successes still mix Provider/A and B. The App now passes B correctly; `handleRequestAckByName` checks Controller permission but omits the request's explicit Provider set. Stop the campaign driver after retaining both failures (driver143); the in-flight User control also completes successfully. This partial cohort is not a final18-case gate.
- **ACK fix**: reject decoded ACKs outside a nonempty pending-call Provider set before status hints, ACK metrics or selection; keep empty-list discovery behavior. Complete any tracked decrypt accounting on rejection. A new native case verifies FirstResponding, RandomSelection and AllSelected refuse another Controller-authorized Provider and select the requested one. Rebuild and full final native/network gates pending.
- **ACK native checkpoint**: build exit0 in8m2.858s; six closures pass; unit183/183 (11998 assertions), integration72/72 (1281 assertions) pass. Final18-case network cohort still required.
- **T021 closure**: final `campaign-ack-final/` at clean994018ac passes18/18 scenarios,188/188 scenario assertions and both dedicated User grant gates. Driver and all CLI exits are0; all33 artifact hashes match disk. Provider target/control successes are17/17 +32/32 and22/22 +65/65; User grants10/10 +24/24 and21/21 +60/60. Planned restart/outage role exits-2 are explicitly covered; other role exits0. `final-network-verification.log` and `evidence/provider-online-grant-20260905.md` are authoritative. Earlier failures are not relabeled.

## 2026-09-05 — NAC-ABE compatibility review exposes dependency boundary defects

- **Area**: Spec179 T020, NAC-ABE Experimental compatibility and callback ownership.
- **Symptoms**: two new real CK fan-out tests abort with memory-access violations when success/error callbacks call `clearCache`; late parameter replies replace new state (two failed assertions); current Authority bytes are returned under an unavailable old version (two failed assertions); wrong/empty CP/KP keys decrypt after another key warmed the singleton cache (four failed assertions).
- **Root causes**: application callbacks invalidate the live waiter-map iterator; ParamFetcher fences neither fetch/validation nor retry generations; Authority reflects a requested exact name instead of its actual generation; the inherited crypto cache is indexed by ciphertext alone. The latter predates both reviewed commits but prior tests manually cleared it before negative-key checks.
- **Repair**: detach CK batches before callbacks and check generation between waiters; fence parameter delivery, validation and retries, and commit decoded/name/digest-checked candidates atomically; construct canonical Authority names; bind the crypto cache to a hashed length-delimited scheme/parameters/private-key/ciphertext tuple. Restore the original no-argument ParamFetcher entry and document class-layout rebuild requirements and silent cancellation semantics. Verification is in progress.
- **Evidence**: `results/spec179-nac-compatibility-20260905/red-{reentry-success,reentry-error,params-authority,cache}.log`; durable report `specs/179-request-scoped-confidentiality/evidence/nac-abe-compatibility-review-20260905.md`.
- **Test/tool findings**: first full NAC run passed41/42; the sole failure was an existing lifecycle probe requiring an unset role variable. Default it to the User role while retaining explicit role validation; CTest also needs the fixture directory and a supported report option. An initial class-layout probe omitted ndn-cxx at link time; relink with its pkg-config libraries. A provisional focused run started before final test linking and used the previous test executable, so it is not the final gate.
- **Inspection failure**: `objcopy --dump-section` without an explicit output object rewrote both input ELF files during an installed-prefix test. Stop that test (exit143, `nac-installed-interrupted.log`), relink from unchanged objects and reinstall to restore the original hashes, then repeat installed-prefix execution. Use read-only ELF readers or disposable input copies for future section comparisons; never inspect live libraries with an in-place tool.
- **Dependent build failure**: the mandatory layout rebuild hit GCC9 `internal compiler error: in ggc_set_mark, at ggc-page.c:1547` in system `basic_string.h`, while compiling `HybridMessageCrypto.cpp` for integration-tests (`ndnsf-build.log`, exit1). Retain completed objects and retry once at the same `-j2`; repeated compiler failure requires the already established Clang10/system-binutils fallback. This is not an executed runtime test failure.
- **ABI rebuild finding**: the retry linked successfully in6m35.140s, but object timestamps showed Controller flow and generic API tests still dated15:24–15:27, before the17:22 parameter-header layout change. A successful incremental link is insufficient: invalidate stale objects for the selected framework/tests/three Apps, retain the exact path list, and rebuild again at `-j2`. Unrelated build targets are excluded. No tests from the stale-object link count as acceptance.
- **Compiler fallback**: after invalidating90 selected stale objects, GCC again crashed in `basic_string.h` and produced a non-constant assembler `.size` expression (`ndnsf-build-fresh-objects.log`, exit1). Stop GCC retries. Configure a new `build-clang-spec179-nac-compat` with explicit Clang10, system binutils, the exact NAC prefix and `-j2`; no cached GCC objects enter that build.
- **Strict compiler finding**: Clang rejects the unused `this` capture in the User status-restore validation-error callback (`clang-build.log`). The mirrored Provider callback has the same unused capture. Remove only those captures; retain `-Werror` and the callback body. This is the only NDNSF runtime source change in the dependency review.
- **Lesson**: tested cancellation must include callback reentry and validator latency; cache tests must retain a warm cache for unauthorized callers. Source-compatible calls do not establish binary-layout compatibility.
- **Native checkpoint**: NAC42/42 full cases,20/20 installed-prefix cases; clean Clang NDNSF build exit0 in21m10.471s, unit182/182 (11971 assertions), integration72/72 (1278 assertions), all six target dependency closures pass. Complete16-scenario MiniNDN rerun pending.
- **T020 closure**: fresh16/16 MiniNDN runs completed with CLI exit0,161/161 scenario assertions and both dedicated User grant gates. All33 artifact hashes match disk and every manifest binds clean de1eb508. Planned Provider restart/Controller outage exits-2 are checked by their scenarios. See `campaign-verification.log` under the evidence root. Provider online first grant remains a separate T021 coverage gap.

## 2026-09-05 — NAC dependency test build retained a removed Boost prefix
- **Area**: T019 dependency regression build
- **Symptom**: after correcting a missing test error-header include and parenthesizing a Boost assertion message, test compilation succeeded but linking required absent `/usr/local/lib/libboost_unit_test_framework.so.1.82.0` (`gates/nac-revocation-red-build2.log`).
- **Root cause**: enabling tests reused stale Boost CMake cache entries in the existing exact-prefix build directory.
- **Fix**: unset only `Boost_*`/`boost_*` cache entries, configure `BOOST_ROOT=/usr` and `Boost_NO_BOOST_CMAKE=ON`; verify all resolved Boost libraries point to system1.71 before rebuilding with `-j2`. Expanded dependency tests subsequently passed14 cases/90 assertions.
- **Lesson**: enabling a previously disabled target can reveal stale optional dependency paths even while the shared-library target builds successfully.

## 2026-09-05 — late NAC content callback survives cache invalidation
- **Area**: Spec179 T019, local NAC-ABE Consumer and OpenABE error boundary
- **Symptom**: WAL campaign retry scenario passes all14 business checks, but Provider/A aborts(-6) with `Specified length is invalid` and uncaught `oabe::_OpenABE_ERROR` immediately after epoch3 installation. The process-exit gate correctly rejects it. Driver stopped(exit143); six completed probes retained, five passed. This is incomplete failed evidence.
- **Mechanism reproduced**: Consumer increments `m_cacheGeneration` only in `clearCache`; neither asynchronous content nor CK completion/error checked it. `nac-consumer-revocation-red2.log` fails5/23 assertions: late content reaches crypto with cleared DKEY and produces the same OpenABE error; late CK refills the cache. CP/KP enum conversion fails2/4 separately. GDB on the preserved old binary/library catches the enum in `constructKeyFromBytes -> parseKeyHeader -> importUserKey -> ABESupport::decrypt`, with the caller blocked in `Consumer::onCkeyData -> decryptContent -> SegmentFetcher` (`gates/nac-late-content-gdb-red.log`). This is the controlled reproduction's stack; the original network process has no stack dump.
- **Fix**: generation-fence both fetch stages' completion/error callbacks and normalize OpenABE enum errors into `NacAlgoError`. Unchanged Consumer tests pass23/23 and CP/KP error/recovery passes4/4; expanded dependency14 cases/90 assertions pass. Committed as NAC-ABE `8b462d0`, exact prefix installed and NDNSF resolution verified. Final NDNSF unit182/182, integration72/72 and full MiniNDN16/16 pass; the retry scenario retains14/14 business checks and all five processes exit0. No wire format, permission ownership, or revocation timing changes.
- **Closure evidence**: `specs/179-request-scoped-confidentiality/evidence/online-authorization-audit-20260905.md` and `results/spec179-online-auth-20260905/campaign-nac-final/`. This final cohort also closes the earlier pending online-grant, initial-DKEY admission, callback, namespace fault and PIB regression entries below; their intermediate failures remain historical evidence.
- **Lesson**: clearing maps and rejecting new requests does not cancel callbacks already admitted under old authority; every delayed stage must retain and verify its originating generation.
## 2026-09-05 — shared MiniNDN PIB reader races another role's startup write
- **Area**: Spec179 online-grant fixture, shared campaign PIB
- **Symptom**: the final-readiness late-grant probe had target21/21 but control0/0: User/A exited1 with `Signing certificate ... does not exist` before its first request. The gate correctly failed. Its certificate/key remained present in the retained PIB. Ordinary grant and eight other completed probes are retained; the driver was stopped (exit143) after this failure, allowing the active ninth probe to clean up normally. This is an incomplete failed campaign, not16-scenario acceptance.
- **Root cause evidence**: the fixture used DELETE journals; installed ndn-cxx PIB SELECT paths treat non-ROW, including BUSY, as absent and configure no busy timeout. The added concurrent-reader regression reproduces `database is locked` under the old setup (`gates/pib-concurrency-red.log`). Lock contention is the supported explanation for the transient App failure; that process did not log SQLite's nested return code.
- **Fix**: initialize the campaign-only PIB in WAL mode before any role starts, require the returned mode to be WAL, and record it in the manifest. Existing writer initialization serialization remains. No host PIB, native library, permission rule or deadline changes. Full harness and a fresh network campaign verify the repair.
- **Lesson**: isolated network namespaces do not isolate an intentionally shared SQLite key store; preserve concurrent signing reads as well as serializing initialization writers.

## 2026-09-05 — base RequestMessage overload bypasses shared admission helper
- **Area**: Spec179 initial-DKEY repair verification
- **Symptom**: first repaired build passed the unwrap callback checks but still failed the two publication checks (6/8, `gates/readiness-green.log`).
- **Root cause**: five convenience/Targeted callers used `prepareRequestControllerVersion`, but the base RequestMessage overload entered `startRequestServiceWithRequestId` directly, duplicating only status/version checks. Checking the helper's callers alone missed that separate entry.
- **Fix**: replace the duplicated base-entry checks with the same shared readiness/status helper. The unchanged regression now passes8/8 (`gates/readiness-base-green.log`); all five native targets rebuilt successfully with `-j2` in12m58.644s (`gates/build-base-admission-green.log`). Expanded unit182/182 and integration72/72 passed (`gates/unit-base-final.log`, `gates/integration-base-final.log`); MiniNDN remains pending.
- **Lesson**: trace from the public failing call to the publication boundary; a helper's caller list does not prove every entry uses it.

## 2026-09-05 — permission renewal admits requests before initial DKEY installs
- **Area**: Spec179 asynchronous User startup and hybrid decrypt callbacks
- **Symptom**: normal grant still failed with target11/13 despite status refresh. NAC Consumer debug shows permission renewal at12s coalescing behind the initial DKEY fetch; the stale result is discarded near15s before a replacement installs. ACKs for the first two requests arrive while the Consumer reports no private decryption key, with no application error callback.
- **Root cause**: removing constructor blocking exposed an implicit prerequisite: Request admission checks permission/status but not initial Consumer readiness. Both runtime hybrid decrypt functions move `onError` into the success closure before constructing the unwrap error callback, leaving the latter empty.
- **Fix**: gate real network Request admission on initial Consumer readiness while retaining LocalMock fixture semantics; copy error callbacks into the asynchronous success/unwrap branches and retain synchronous exception reporting. The real-constructor regression reproduced four failed assertions (publication1 instead of0, errors0 instead of1); `gates/readiness-red.log`, exit201. The pre-fix binary is retained. Fixed rebuild/regression is pending.
- **Evidence**: `grant-crypto-diagnostic/user-B.log`; earlier `campaign-grant-final` also retains one control timeout at the grant/status transition. Exact-version rejection is expected during distributed convergence; do not claim instantaneous default-policy convergence or hide that failed row.
- **Lesson**: asynchronous construction must replace former implicit prerequisites with explicit admission checks; moving a shared callback into one branch can silently disable another branch.
- **Probe setting**: normal grant now uses the existing 250ms status refresh knob (four opportunities per 1rps request interval); late grant retains1s. Every failed row remains counted. These measured settings do not establish zero interruption with default status-refresh timing.

## 2026-09-05 — grant control failures were not included in the gate
- **Area**: Spec179 grant-only control and status-convergence evidence
- **Symptom**: normal renewal probe failed with target12/13 and control12/24 successes. The collector exposed control successes only, so the twelve control failures were absent from gate conditions.
- **Root cause**: successful-only control projection lacked a companion failure count. Separately, this grant probe disabled scheduled status refresh: providers learned epoch2 from target traffic while User/A remained epoch1, and the first target request's ACK waited on Provider refresh. Exact-version rejection remains required; a short convergence probe cannot assume all peers instantly discover grant-only changes.
- **Fix**: record every control row and require zero control failures; the new regression fails before the change. Use the existing 1s status refresh knob for the normal grant probe, matching the corrected late-grant case and other bounded revocation tests. This is an explicit experiment/deployment setting, not a claim of instantaneous convergence under production defaults. Retain the 12/24 negative run; recheck earlier late-grant raw rows against the stronger control rule. Final normal grant rerun pending.
- **Ref**: `campaign-renewal/grant-only-advance`; `gates/harness-control-red.log`; T018.
- **Lesson**: key reuse and status-version convergence are separate conditions; count all control outcomes, not only successful ones.

## 2026-09-05 — asynchronous startup exposes three MiniNDN timing assumptions
- **Area**: Spec179 grant and offline-rejoin probes (T018)
- **Symptom**: rebuilt full campaign finished 13/16, exit 1. Normal grant had no renewal and zero granted requests; late grant had 21/21 successes but no exhausted permission retries; offline User installed epoch 2 before reaching epoch 3.
- **Root cause**: old constructor blocking implicitly deferred permission discovery until grant. Once constructors return, an empty permission response completes normally, so absence of a grant does not force transport retry exhaustion. Fixed SIGCONT timing relied on old startup skew and could precede the actual epoch-3 revoke. The late control also needed scheduled status renewal for its 60-second window.
- **Fix**: both grant probes use explicit App refetch; late probe drops outgoing UDP only inside the isolated user-b namespace until observed final permission timeout, then removes the exact rule in finally. A namespace guard refuses host execution. Keep control statuses renewed through existing knobs. Offline SIGCONT waits for the actual revoke marker. No failed rows or ordering checks are removed. Three focused MiniNDN reruns pending.
- **Ref**: `results/spec179-online-auth-20260905/campaign-final/`; `permission-startup-loss.json` in the corrected late run records both fault boundaries. Native binaries and libraries are unchanged for the rerun.
- **Lesson**: test prerequisites must be observed, not inferred from constructor latency, empty responses, or estimated mutation deadlines.

## 2026-09-05 — stream retry timing assertion fails alongside compilation
- **Area**: Spec179 final integration verification / shared-host load
- **Symptom**: `NormalStreamRetriesOneSuppressedEventFromProviderIms` completed successfully but recorded two retries instead of exactly one; the expanded gate returned 201 (70/71 cases, 1269/1270 assertions).
- **Root cause**: the test uses a 100ms Interest lifetime; concurrent `-j2` App compilation is a plausible scheduling cause, not yet established by a controlled load experiment. No source change to the stream implementation occurred in this repair.
- **Fix**: retained `gates/integration-final.log`, finished compilation and reran without compilation load. The isolated case passed immediately (12/12 assertions, exit 0); full isolated integration gate passed 71/71 cases and 1270/1270 assertions (`gates/integration-final-isolated.log`). No retry assertion or timeout was weakened.
- **Lesson**: independent binaries avoid link races but do not isolate timing-sensitive tests from shared CPU pressure. Run the final timing gate without compilation.

## 2026-09-05 — unprovisioned runtime cannot reach online permission renewal
- **Area**: Spec179 User/Provider construction and online grant (T018)
- **Symptom**: late-grant MiniNDN probe failed the App_User readiness deadline; no permission fetch or App refetch marker appeared, only repeated Waiting for decryption key lines. The original first-grant scenario began the App only after grant unlocked construction.
- **Root cause**: both real constructors synchronously pump their Face until Consumer has a DKEY. An identity with no grant cannot finish construction to call the App-owned permission API. This corrects the earlier startup-delay hypothesis: the relevant delay was the DKEY gate, not slow RSA initialization.
- **Fix**: begin the existing asynchronous Consumer fetch and return with an explicit bootstrap-pending marker. Keep all permission/status/key checks. Timed real User/Provider constructor coverage passes 8/8 assertions with zero unauthorized publication/execution; final late-grant network rerun remains pending.
- **Ref**: campaign/grant-after-permission-exhaustion; ServiceUser/ServiceProvider constructors; UnprovisionedRuntimesConstructAndRemainUnauthorized.
- **Lesson**: an application-owned recovery API is unusable if construction blocks waiting for the condition that API must recover.

## 2026-09-05 — one-second benchmark drain truncates delayed valid responses
- **Area**: Spec179 MiniNDN workload shutdown
- **Symptom**: corrected-duration inflight-revocation run reported two unsuccessful unaffected-user requests near workload end, despite ten successful post-revoke requests.
- **Root cause**: the launcher allowed only one second of drain for a three-second Provider delay and five-second request timeout. Open-loop finalization emitted incomplete rows before valid in-flight work could finish.
- **Fix**: drain six seconds (five-second request timeout plus margin), use at least 35-second role windows for the standard scenarios, retain all failures and rerun. No success filter is added.
- **Ref**: campaign/inflight-revocation; App_User drainDeadline; T018.
- **Lesson**: measured-window completion and process survival must include the entire request drain budget.

## 2026-09-05 — failed withdrawal bypassed by grant or same-target retry
- **Area**: Spec179 Controller pending ABE rotation
- **Symptom**: the new real Controller regression produced 10 failed assertions: same-target retry left the old ABE pair; grant removed the revocation during injected rotation failure; direct recovery produced an equal-version conflicting status rejected by RevocationState.
- **Root cause**: duplicate-target return preceded reconciliation; grant never checked pending rotation; reconciliation reused a ControllerVersion whose old parameter identity could already be published.
- **Fix**: reconcile before duplicate handling and grant mutation, persist a newer epoch before recovery crypto work, return successful completion for the pending same-target retry, and retain ordinary completed-duplicate no-op behavior. One-shot injection and a repeated App revoke support the matching MiniNDN scenario.
- **Ref**: T017; PendingRotationFencesGrantAndPreservesImmutableStatus; `results/spec179-online-auth-20260905/gates/controller-red.log` (exit 201, 10 failed assertions) and `controller-green.log` (exit 0, 36/36 assertions, including old/replacement DKEY decryption). MiniNDN `campaign/revocation-rotation-failure-retry`: 14/14 checks pass, epoch 2 -> 3, affected denial throughout, 16/16 unaffected post-recovery calls succeed.
- **Lesson**: failure recovery is an authorization mutation too; test every public entry and preserve already published immutable status identities.

## 2026-09-05 — late-grant probe initially missed its timing contract
- **Area**: Spec179 MiniNDN T018 probe
- **Symptom**: first late-grant run completed but gate failed (exit 4): user/B started after the Controller grant, no startup timeout exhaustion occurred, and the result exporter omitted the new scenario's grant evidence.
- **Root cause**: startup outlasted the 12-second grant offset; the later probe identified the constructor DKEY wait (see the entry above), superseding the initial RSA-delay hypothesis. One scenario-name equality remained in the evidence return despite sharing the grant collector.
- **Fix**: grant evidence now follows the grantOnlyAdvance configuration for both scenarios; added an exporter regression. Increased grant/renewal/workload windows and require measured exhaustion < grant < refetch <= first invocation plus post-refetch unaffected successes. First run retained as a failed timing probe.
- **Ref**: results/spec179-online-auth-20260905/late-grant-first; 12 launcher tests pass. Corrected network rerun pending.
- **Lesson**: launch offsets are assumptions; acceptance must verify event ordering from observed timestamps.

## 2026-09-05 — MiniNDN open-loop milliseconds interpreted as seconds
- **Area**: Spec179 launcher workload lifetime
- **Symptom**: a run configured for 16 seconds kept enqueueing until the process lifetime killed it; late-grant-first user/A enqueued for about 51 seconds despite the nominal workload/count.
- **Root cause**: requestDurationMs was passed directly to App_User --duration, which uses std::chrono::seconds; --count applies to closed-loop mode and does not cap this open-loop workload. Teardown could therefore truncate in-flight requests, previously hidden by the grant collector.
- **Fix**: round milliseconds up to integer seconds at the App_User command boundary. The final late-grant probe uses 60-second workloads for both users, spanning explicit renewal at 40 seconds after App startup; the fault/retry scenario explicitly spans both mutation events and drains before process shutdown.
- **Ref**: examples/App_User.cpp openLoopDurationSeconds and measurementStopAt; run_request_scoped_confidentiality.py user_command; T018.
- **Lesson**: verify units at the actual CLI consumer and distinguish open-loop duration from closed-loop count.

## 2026-09-05 — online authorization audit detects censored MiniNDN failures
- **Area**: Spec179 MiniNDN evidence and exit status
- **Symptom**: a success plus a failed request while providers were alive was counted as one successful row; a completed run with gatePassed=false returned exit code 0.
- **Root cause**: the grant collector used the earliest provider log timestamp as a termination cutoff and dropped failure rows; main checked process completion alone.
- **Fix**: retain all terminal rows, allow bootstrap/workload/drain in the grant scenario lifetime, and require gatePassed=true for exit 0. Two regression cases reproduced both defects before the fix; the 11-case launcher suite passed afterward.
- **Ref**: tests/minindn/test_request_scoped_confidentiality.py; /tmp/spec179-online-auth-harness-red.log and harness-green.log; T018.
- **Lesson**: process completion is not a security gate; never infer teardown from a first log or silently discard negative evidence.
- **Real-run confirmation**: reprocessing `results/spec179-online-auth-20260905/baseline-grant` retained 16 requests with 14 successes and 2 timeouts. The old collector had reported 14/14. The campaign driver now propagates failed gates, uses fresh output directories, and refuses to overwrite retained scenarios; each new manifest records revision, working diff and executable/library hashes.

## 2026-09-05 — online authorization audit preflight and test authoring corrections
- **Area**: Context Mode and Controller regression fixture
- **Symptom**: active authority hashes were stale; project query guard rejected a low-entropy identifier and then an identifier absent from its query; a new C++ regression did not compile.
- **Root cause**: prior Spec edits were not indexed; malformed guard arguments; makeServiceRevocation takes const char* rather than std::string.
- **Fix**: reindexed canonical authority documents, verified active health, corrected and reran the guarded project query; passed the temporary URI through c_str for the immediate copying helper call.
- **Lesson**: use file-backed checkpoints after retrieval failures and verify fixture signatures before writing a regression. An initially rejected query is not accepted authority.

Append-only engineering failure record. Rule (AGENTS.md): every failure that
costs non-trivial debugging MUST be appended here **in the same checkpoint
commit that fixes or records it**. New tasks MUST read the recent entries as
part of task context. Format per entry:

```text
## <date> — <one-line symptom>
- **Area**: <spec or module>
- **Symptom**: <what was observed>
- **Root cause**: <why>
- **Fix**: <what changed / workaround>
- **Ref**: <commit, evidence file, or script>
- **Lesson**: <one line to carry forward>
```

## 2026-09-05 — git index duplicate entries wrote a corrupted tree
- **Area**: tooling/git
- **Symptom**: `git add -A` with a pathspec containing a comma staged
  duplicate index entries; the resulting commit tree had `duplicateEntries`
  + `treeNotSorted` (`git fsck` errors), and a rename-detection warning
  "duplicate destination".
- **Root cause**: comma pathspec left the index with unordered/duplicate
  stage entries.
- **Fix**: `rm .git/index && git reset --mixed <last-good>` rebuilt the
  index; re-staged with explicit paths; `git prune --expire=now` dropped
  the bad commit.
- **Ref**: NDNSF commits `32b1fc23` (re-created) replacing the bad
  `8fc879ce`.
- **Lesson**: never use `git add -A` with comma/odd pathspecs; verify
  `git ls-files | sort | uniq -d` is empty before committing.

## 2026-09-05 — stale campaign-summary.tsv contradicted final MiniNDN results
- **Area**: Spec179 evidence
- **Symptom**: `results/spec179-minindn/campaign-summary.tsv` showed many
  scenarios `gatePassed=False` while per-scenario `result.json` said true.
- **Root cause**: the TSV predated the final campaign runs (13:08 vs runs
  15:47–17:18) and was never regenerated.
- **Fix**: regenerated from the final `result.json` files (14/14
  `gatePassed=True`).
- **Ref**: Spec179 `evidence/post-implementation-audit.md` R179-A4.
- **Lesson**: derived summary artifacts must carry a timestamp and be
  regenerated, or deleted, after the runs they summarize.

## 2026-09-04 — MiniNDN campaign caught two admission defects
- **Area**: Spec179 runtime
- **Symptom**: S9 — `RequestServiceTargeted` issued versionless requests;
  S10 — `requestServiceStreamingBytes` discarded the admission result so a
  denied stream start logged STARTED.
- **Root cause**: missing version binding on the Targeted request path;
  ignored revocation admission result on the stream start path.
- **Fix**: fixed in `ServiceUser.cpp`; rebuilt; genuine campaign rerun
  green.
- **Ref**: `evidence/minindn-campaign-20260904.md`, NDNSF commit `e7ea0a74`.
- **Lesson**: the cross-process campaign is the admission-boundary oracle;
  component tests did not catch either defect.

## 2026-09-03 — OpenABE mixed-generation decrypt returns garbage
- **Area**: NAC-ABE/OpenABE crypto
- **Symptom**: decrypting new-generation ciphertext with a retained old
  DKEY could return garbage plaintext instead of throwing.
- **Root cause**: OpenABE generation mismatch does not always fail loudly.
- **Fix**: Spec179 test assertions use `decryptFailsClosed` (throw **or**
  recovery failure both accepted); RV-U20 mixed-generation matrix with
  fresh ciphertext per case (the ABESupport singleton CK cache would
  otherwise mask the mismatch).
- **Ref**: Spec179 `evidence/runtime-revocation-lifecycle-20260904.md`.
- **Lesson**: crypto-negative assertions must accept "recovered garbage"
  as failure; never reuse a successfully-decrypted ciphertext in a
  generation-mismatch case.

## 2026-09-03 — NAC-ABE stale DKEY after grant-only policy replacement
- **Area**: NAC-ABE dependency
- **Symptom**: target refresh could receive the previous complete DKEY.
- **Root cause**: DKEY segments published with `FreshnessPeriod=4s`; the
  unversioned `MustBeFresh` discovery Interest then hit a still-fresh
  Content Store copy of the old policy.
- **Fix**: DKEY segments now publish with `FreshnessPeriod=0`; exact
  versioned segment names remain retrievable.
- **Ref**: NAC-ABE `Experimental` branch commit `b1c9c4f` (not pushed).
- **Lesson**: any in-place policy replacement needs freshness discipline
  on unversioned discovery names.

## 2026-09-03 — versioned exact public-params Interest could never match
- **Area**: NAC-ABE dependency
- **Symptom**: after status installation,
  `refreshPublicParameters` with the exact
  `/PUBLIC-PARAMS/<ABE-TYPE>/v=<version>` name (CanBePrefix=false) timed
  out repeatedly.
- **Root cause**: `AttributeAuthority::onPublicParamsRequest`
  unconditionally appended `<ABE-TYPE>` + version to the Interest name,
  producing a Data name that can never satisfy the exact request.
- **Fix**: detect an already-versioned name and do not append again;
  ParamFetcher binds expected name/digest.
- **Ref**: NAC-ABE `Experimental` commit `b1c9c4f`.
- **Lesson**: producer-side name derivation must mirror every Interest
  shape the consumer may legally send.

## 2026-09-02/04 — build and test-environment traps (Spec179 baseline)
- **Area**: build/tests
- **Symptom** (three independent traps):
  1. GCC 9 ICE on `data-enc-dec.cpp` — NAC-ABE must be built with
     `clang++-10`.
  2. Two concurrent waf builds in different out dirs conflict on the
     shared lock and one is killed silently.
  3. After a full-suite SIGSEGV, Boost.Test keeps running and the
     residual process disturbs later timing runs — kill residuals before
     re-running.
- **Fix**: documented build recipe (clang++-10, single build at a time,
     kill-then-retest).
- **Ref**: Spec179 `evidence/restore-fixes-20260904.md`,
     `evidence/regression-red-green-20260904.md`.
- **Lesson**: environment traps must be recorded next to the build
  recipe, not rediscovered per session.

## 2026-09-02 — DummyClientFace hangs and LocalMock DKEY reattach
- **Area**: tests
- **Symptom**: `processEvents` blocked forever on a fully idle face;
  pump-driven LocalMock members could not verify DKEY segments.
- **Root cause**: deferred DKEY reattach had no bound when the face went
  idle.
- **Fix**: bounded retry (250 ms × 20) for deferred DKEY reattach;
  request-pump fixture extended to pump the AA face (Spec179 remounts).
- **Ref**: Spec179 baseline fixes in NDNSF commit `e7ea0a74`.
- **Lesson**: every deferred async retry needs a bounded schedule or an
  idle-face test can deadlock the whole suite.

## 2026-09-07 — Controller receipt conflicted with read-only configuration mount

### Readiness boundary review follow-up

- The old receipt comparator collapsed duplicate artifact rows into dictionaries, hiding incorrect cardinality. MiniNDN and Tiger now use one shared pure validator that rejects duplicates, malformed rows, missing artifacts and changed digests.
- Host-side Worker readiness must not import an installed `/opt/...` SIF path. The pure receipt checker lives in the frozen runtime bundle; only in-container preparation imports installed application owners. A focused regression prohibits that host import. These are source/fixture findings, not claimed successful signed-network tests.

### Preparation mount review follow-up

- Source review found that a strict empty `/identities` check conflicts with the shared Apptainer command's pre-created `root` HOME. The guard now allows only that empty directory and rejects any old content; a focused regression covers both cases. This was fixed before any SIF launch, not observed as a cluster failure.
- Private preparation inputs now have an explicit read-only `/inputs` mount restricted to offline preparation (no node mount/GPU/ordinary worker). Final operator gating and actual SIF execution remain pending.

- Symptom: source audit found the maintained publication function writes `runtime-publication-receipt.json` beside its input; Spec183 passes `/config/runtime-publication.json` under a read-only mount. Real deployment would fail after publication.
- Cause: an old MiniNDN writable-directory assumption crossed into the SIF launch contract; process-argv tests alone did not exercise receipt I/O.
- Fix: explicit optional receipt-output CLI on the original Controller; Tiger launch passes `/output/runtime-publication-receipt.json`. Explicit paths reject overwrite/input alias/symlink/traversal; legacy default remains for older runners. New source seal/SIF required.
- Evidence: verbatim function with simulated transport tests actual filesystem receipt writes; 338 focused tests passed. No real signed publication or SIF gate is claimed.
- Lesson: review output side effects of every invoked application, not only argv/input mounts; immutable config and writable evidence must be separate.

## 2026-09-07 — Spec183 public-recipient seam native import gate

- Symptom: two new actual User-seam tests failed during setup, before grant execution.
- Cause: the maintained User imports the host `ndnsf._ndnsf` extension, which is unavailable in this checkout/runtime; the Python partially-initialized-module text is not evidence of a newly introduced circular dependency.
- Resolution/status: preserved both tests without skip or fake extension. T008 native build must rerun the full seam suite. Public-key decoding and launch-boundary checks pass in the 311-test focused suite, but do not close this native integration requirement.
- Lesson: pure security/launcher tests cannot establish application import or protected grant integration. Preserve exact failure scope and require the real native environment.

## 2026-09-02 — SegmentFetcher infinite fetch on discovery Data
- **Area**: Core `ServiceProvider::replyFromIMS`
- **Symptom**: SegmentFetcher kept requesting segments until timeout.
- **Root cause**: discovery Data served from IMS lacked `FinalBlockId`.
- **Fix**: forward to the last contiguous IMS segment and set
  `FinalBlockId`.
- **Ref**: Spec179 baseline fixes.
- **Lesson**: any segmented Data served to a SegmentFetcher must carry a
  terminal marker or the fetch is unbounded.
## 2026-09-07 — Spec183 V3 lifecycle candidate identity mismatch

Second follow-up: verified YOLO catalogue body digest is now retained by the
splitter and emitted in V3 GRAPH_READY. Canonicalization matches signature
verification (excludes signature envelope). Real journal-to-collector file
round-trip passes; actual native adapter/model qualification remains pending.

Follow-up: adapter resolver now recomputes the selected runtime digest against
registered conversions and requires exactly one match. V3 emission uses the
resolved catalogue identity; no runtime digest semantics change. Four isolated
production-kernel regressions pass; native verification still pending. Audit
also found GRAPH_READY omits required catalogueDigest; keep T006 open until
that field comes from the verified catalogue owner.

- Symptom: V3 PLACEMENT_DECISION candidateId is the digest, while prepared
  offer trust and numerical evidence use the catalogue candidate name.
- Cause: V3 SplitCandidate exposes a content digest but no catalogue label;
  the planner writes that digest to both fields and the User forwards it.
- Status: discovered by source audit before runtime qualification; NOT fixed.
  Added strict lifecycle rejection and regression for this mismatch. T006/T007
  remain open until a trustworthy mapping is implemented.
- Lesson: validate the actual writer against the collector contract before
  launching a GPU job; do not weaken evidence binding to make a trace pass.
## 2026-09-07 — Native evidence serializer/collector type mismatch

Regression uncovered a separate network boundary defect: missing-peer expiry
sometimes raised Python 3.8 asyncio.TimeoutError, not the built-in TimeoutError
used by the explicit inner deadline. Unified exchange() timeout normalization;
added a deterministic outer-wait expiry regression. Initial full run was
517 passed/1 failed, not PASS. Focused network+observation follow-up: 43 passed.

- Symptom: the old Spec180 collector rejects actual native bool/uint64 JSON
  scalar strings and assumes `/example/provider/<role>` identities.
- Cause: executionEvidenceToJson uses Boost PropertyTree, while collector
  fixtures used Python typed JSON; namespace assumptions were not deployment-bound.
- Resolution: Spec183 decoder recognizes only canonical values in the known
  native fields, and validator binds actual expected Provider/PID/request/plan.
  29 focused tests pass; native serializer/runtime and full collector pending.
- Lesson: inspect producer serialization, not only synthetic collector fixtures.
## 2026-09-07 — Cross-phase peer failure visibility

- Symptom: normal owner originally watched only completion failure files after
  startup; a peer failing before its completion factory returned could only
  publish startup failure, delaying detection until a timeout.
- Fix: completion liveness checks read startup failure records without using
  its elapsed deadline; finite User peer path watches the common startup lane.
- Evidence: concurrent real-file barrier normal/failure regressions plus an
  earlier-phase-only failure test; application/runtime remain doubles.
- Lesson: phase-local coordination must still observe earlier ownership
  failure channels throughout the run.
## 2026-09-07 — Missing dependency trace enablement in YOLO worker

- Symptom: qualification needs paired DATA_V1 dependency observations, but
  the Worker launch did not enable either native dependency tracing variable.
- Fix: enable NDNSF_DI_DEPENDENCY_OBJECT_TRACE=1 explicitly; add launch assertion
  and source-format paired publish/fetch validator tests.
- Remaining: User has not retained the safe public sealed dependency contract;
  collector cannot derive expected edges from observations themselves.
- Lesson: verify both producer instrumentation and independent expected
  contract before claiming edge coverage from successful final responses.
## 2026-09-07 — V3 numerical receipt used carrier rather than execution digest

- Symptom: V3 PLAN_SEALED/native assignments use PlanSealerV3 execution digest,
  while YOLO User wrote SealedCollaborationPlan.plan_digest to numerical evidence.
- Cause: the outer carrier digest includes assignment payloads and is a
  different object; isolated tests supplied one generic plan string.
- Fix: AutomaticInferenceHandle.execution_plan_digest exposes the coordinator's
  existing runtime plan binding, with strict digest validation and legacy/V2
  carrier fallback only when no explicit metadata binding exists. YOLO uses it.
- Evidence: property kernel distinguishes carrier/runtime, rejects invalid
  explicit bindings; numerical production-tail fixture now separates names.
  Native integration remains pending.
- Lesson: identify what each digest covers before joining evidence; similarly
  named plan fields are not interchangeable.
# 2026-09-07 Spec183 retained receipt semantics gap

- Symptom: seven focused mutations of a hash-consistent retained node receipt
  were accepted despite invalid cleanup or incomplete request coverage. This
  was a component audit before any native/SIF/Tiger qualification.
- Root cause: the initial offline reader checked receipt/log content binding
  and services, but relied on producer-side cleanup/request checks and did not
  recompute the stored cleanup summary.
- Fix: share validate_cleanup_records between live and offline paths, preserve
  live ownership checks, and revalidate the frozen request inventory offline.
- Lesson: a matching artifact hash proves content identity, not validity of
  the reported execution; validate semantics at the final consuming boundary.
# 2026-09-07 Spec183 host/container PID identity mismatch

Follow-up: Worker nonce/witness launch, receipt v3 and live/offline readers are
now wired. The real namespace/reader regression shows raw host-PID validation
fails and nonce-bound namespace-PID validation succeeds. Host PID remains the
cleanup identity. Exact-SIF acceptance is still pending; this resolves the
code path and focused regression, not the full runtime qualification.

- Symptom: planned native evidence validation compares Provider getpid() with
  subprocess.Popen(apptainer).pid. The configured --containall isolates PID;
  the values need not match. Real local unshare user/PID namespace test
  demonstrated different launcher and execed application PIDs.
- Root cause: earlier launcher tests replaced Apptainer with ordinary exec,
  preserving the host PID and missing namespace behavior.
- Remediation in progress: a nonce/role-bound launch witness runs inside the
  container, emits its PID, then execs the Provider. Real exec and namespace
  tests pass; Worker/receipt/collector wiring and exact-SIF acceptance remain
  pending. Do not loosen PID validation or remove containment to hide it.
- Lesson: test the namespace boundary explicitly; host process ownership and
  native process identity are distinct facts that need an observed link.

# 2026-09-07 Spec183 device validator not called by retained collector

- Symptom: a CPU record claiming CUDA_VISIBLE_DEVICES=0 passed the actual
  retained receipt/native/profile reader, despite a standalone device checker.
- Root cause: the device check had focused tests but was not wired into the
  consuming role collector; native/profile agreement alone did not check it.
- Fix: retained request/dependency/role path now validates device claims and
  requires independent per-node UUID/launch selector inputs for CUDA roles.
- Evidence: cpu-gpu-exposure first failed DID NOT RAISE; reader regressions now
  reject CPU exposure and missing/mismatched GPU bindings. Synthetic records
  do not prove a physical allocation; preflight producer remains pending.
- Lesson: test rejection at the consuming boundary, not just helper behavior.

# 2026-09-07 Spec183 finite command confused with native Provider

- Symptom: a valid finite nfdc command borrowing BackboneNeck HOME fails node
  receipt creation with NODE_RECEIPT_LAUNCH_NONCE. Reproduced before GPU probe
  wiring, using the actual receipt writer and cleanup record contract.
- Cause: witness checks keyed only on role name, not persistent vs finite
  invocation. Only the persistent native Provider exec emits that witness.
- Fix: writer and reader require witnesses for persistent Provider processes;
  finite commands keep their owned PID/exit/log/cleanup bindings without a
  fictitious Provider witness. Unexpected finite launchNonce is rejected.
- Lesson: identity/HOME ownership does not identify the executable lifecycle;
  include management and readiness invocations in receipt regressions.

# 2026-09-07 Spec183 Slurm exit0 does not imply an existing allocation

- Observation: live read-only `scontrol --json show job 999999999` and
  `show step 999999999.0` returned exit0, errors=[], warnings=[], but jobs=[]
  and steps=null on iTiger Slurm24.05.2. No job was launched for this check.
- Risk: a command-status-only allocation gate could accept a nonexistent job.
- Guard: the new task binding reader requires exactly one matching running
  job/step with correct owner, submission comment and topology; fixtures test
  both observed empty forms and verify capture stops before downstream work.
- Lesson: validate scheduler payload semantics and expected identity, not only
  process exit status. Positive source-shaped fixtures are not live acceptance.

# 2026-09-07 Spec183 native collector accepted absent model identity

- Symptom: nine malformed/missing/wrong-role model and artifact identity
  mutations were accepted by validate_native_observation (red regression).
- Cause: the collector checked execution/request/backend fields but omitted
  fields emitted by executionEvidenceFromRunnerSpec. Its shared synthetic
  observation fixture likewise omitted modelDigest/artifactDigests.
- Fix: require canonical SHA256 model and role artifact fields in the real
  native observation reader, with exact single-role artifact ownership; update
  the fixture including the native Merge role.
- Boundary: shape validation is not certified-model binding. Current native
  preparation can use planDigest when modelManifestDigest is absent. T006 must
  independently bind signed model/assembly identities before final PASS.
- Lesson: fixtures must preserve actual required native fields; matching two
  self-reported logs does not independently prove complete graph execution.

# 2026-09-07 Spec183 public model binding was not carried into retained files

- Symptom: the typed projection emitted role/model identity only internally;
  the retained User envelope remained v1 and the collector had no model binding.
- Cause: the projection and collector evolved independently, so an old envelope
  could be treated as a complete dependency contract.
- Fix: version the envelope as v2, emit per-role modelManifestDigest and
  artifactDigest, reject incomplete identity at the User boundary, and require
  exact canonical digests and role ownership at collection.
- Boundary: this remains structural evidence. Signed package verification and
  optimized graph coverage are separate gates and remain open.
- Lesson: every evidence field must be carried through producer, retained file,
  reader, and external expected identity before it can support a final verdict.

# 2026-09-07 Spec183 graph coverage lacked an optimized-node boundary

- Symptom: native/profile checks could compare assignments internally but had no
  independent expected graph vocabulary; raw ONNX node counts would be invalid
  after ORT fusion.
- Cause: the collector had model identity and backend checks but no external
  certified graph mapping.
- Fix: add a fail-closed `tiger-yolo-certified-graph-v1` join requiring exact
  role model/artifact bindings, backend, and optimized node-name coverage.
- Boundary: the API is optional for existing component fixtures; the final
  operator must supply it and reject absence before declaring T006 PASS.
- Lesson: optimization-aware coverage requires a separately certified expected
  vocabulary, not a count inferred from runtime events.

# 2026-09-07 Spec183 had no final normal-verdict completeness boundary

- Symptom: component readers could validate individual lifecycle, numerical,
  execution, and graph records without proving that every registered request
  in a normal case had passed all required checks.
- Cause: evidence collection grew incrementally and had no final request-count,
  role-set, and case-specific device gate.
- Fix: add `collect_normal_verdict` as the public collection boundary and
  `finalize_normal_verdict` as its fail-closed terminal gate. Together they
  require the exact warmup/measured schedule, four certified roles, matching
  graph digest, and expected device set before emitting
  `tiger-yolo-final-verdict-v1`.
- Boundary: this is still a component-only collector; the operator CLI is not
  wired to real retained paths, and T006/T007 plus native/SIF/Tiger validation
  remain open.
- Lesson: component validators need an explicit final completeness gate before
  any normal experiment can be reported as PASS.

# 2026-09-07 Spec183 final verdict trusted role names without role evidence

- Symptom: a final component result with the four expected role names but empty
  role records could satisfy the initial completeness check.
- Cause: the terminal helper checked role-set coverage but not each role's
  retained component qualification, dependency qualification, or certified
  graph qualification.
- Fix: require `RETAINED_ROLE_COMPONENT_ONLY`,
  `DEPENDENCY_COMPONENT_ONLY`, and `CERTIFIED_GRAPH_COMPONENT_ONLY` at the
  final boundary; add negative mutations for each forged record.
- Boundary: all evidence remains source-shaped component evidence; no native,
  SIF, allocation, or Tiger execution is implied.
- Lesson: terminal aggregation must validate both identity coverage and the
  qualification of every child component.

# 2026-09-07 Spec183 host-gate evidence kind rejected valid receipt

- Symptom: the first positive host-gate validator test failed with
  `YOLO_HOST_GATE_FILE_RECORD:evidence.cleanup`.
- Cause: the evidence wrapper's semantic `kind` field was passed into the
  lower-level exact `{path, bytes, sha256}` file-record validator.
- Fix: validate `kind` at the case layer and remove it before binding the
  evidence file.
- Lesson: layered receipt validators must separate semantic wrapper fields
  from content-identity fields.

# 2026-09-07 Spec183 dispatch changed the legacy Apptainer command boundary

- Symptom: moving the Apptainer version probe after all validation made four
  established Spec175 builder tests fail because they intentionally require
  the legacy `version` probe before the later definition/host rejection.
- Cause: the new zero-side-effect requirement was applied to the old
  Spec175 dispatch instead of only the new Spec183 receipt path.
- Fix: preserve the Spec175 probe order and defer the version probe only for
  `spec183-yolo`, after source/receipt/definition/preflight validation.
- Lesson: a new workload gate must add a separate command-boundary contract;
  it must not silently rewrite an established release path.

# 2026-09-07 Spec183 SIF probe used a read-only home

- Symptom: importing `ndnsf` inside the historical base SIF aborted with a
  filesystem error while creating `/home/tianxing/.ndn`; the same native
  import succeeded when the container received a temporary writable home.
- Cause: the Spec183 exact-SIF probe used `--no-home` and only attempted to
  override `HOME` through the environment. Apptainer preserved the passwd
  home, so ndn-cxx initialization could not create its security directory.
- Fix: run each SIF probe with a fresh private `--home` directory and no host
  home exposure; add a command-boundary regression for the isolated home.
- Lesson: native import/ldd probes must provide the same writable per-role home
  contract as the real worker, not merely set an environment variable.

# 2026-09-07 Spec183 builder test used a host receipt as a source seal

- Symptom: the valid component-receipt dispatch test stopped at
  `LOCAL_SIF_SOURCE_SEAL_DIGEST_MISMATCH` instead of reaching the intended
  missing Spec183 SIF-preflight gate.
- Cause: the host-gate fixture's minimal source document is not the complete
  archive-backed source seal consumed by `validate-local-sif-source.py`.
- Fix: keep the host receipt and build source seal separate; bind the receipt
  to a real archive-backed test source seal before invoking the builder.
- Lesson: a component qualification receipt and a build-input source seal are
  different contracts and must never be substituted for one another.

# 2026-09-07 Spec183 dispatch had no SIF/ABI preflight boundary

- Symptom: the initial dispatch would mark the Spec175 SIF preflight
  `NOT_APPLICABLE` for YOLO and could reach build/record without a Spec183
  native import/ldd/entrypoint check.
- Cause: the new receipt gate was added without a workload-specific final SIF
  preflight owner.
- Fix: require the real `ndnsf-di-spec183-preflight` in two phases (sealed
  inputs before Apptainer, exact SIF after build); missing preflight fails
  closed with zero Apptainer calls.
- Lesson: a new workload dispatch may not disable an existing release gate
  unless an equivalent workload-specific gate is present.
# 2026-09-07 — Spec183 interrupted Slurm dispatch review

Symptom: an uncommitted launcher patch had passing mocked submission tests,
but requested a case's own success receipt before first execution; emitted
900 seconds as bare Slurm time 900 (minutes); fixed every case at two nodes;
and selected the working-checkout wrapper without a typed GPU request.
The proposed staging gate accepted generic PASS/READY fields without binding
the actual promoted files. No Slurm job was launched.

Cause: tests mocked qualification and asserted successful submission without
checking the case sequence, Slurm units or frozen runtime boundary.

Fix: use preceding-gate prerequisites, explicit HH:MM:SS, case node counts,
typed GPU GRES, and the frozen wrapper. Keep actual dispatch closed pending
semantic staging/receipt validation and runner/reconciliation implementation.
74 focused operator/profile/journal tests passed in 9.66s.

Lesson: test the first execution of each gate and exact allocation semantics;
mocked PASS fields cannot establish staging or permit a real sbatch call.
# 2026-09-07 — Spec183 cached verdict bypassed evidence reanalysis

Symptom: collect returned success after evidence removal, evidence mutation,
forged qualification or an oracle rejection when verdict.json already existed.
Five focused mutation tests reproduced the false success before the repair.

Cause: the existing-verdict fast return verified only run/candidate/schema/status
and bypassed handoff loading and the authoritative collector.

Fix: re-run collection for existing verdicts, compare the full recomputed
result, reject disagreement and preserve the original verdict bytes.
35 operator tests passed in 8.40s, including unchanged reanalysis and all five
failure cases. Dispatch qualification is mocked in these component tests;
this is not evidence of a real Tiger/model run.

Lesson: immutable summaries preserve history; they do not replace validation
of their retained evidence on later collection.
# 2026-09-07 — Spec183 model-input blocker conflated two manifest roles

Symptom: continuation repeatedly waited for a signed replacement of the
legacy external model-manifest summary even though the local YOLO package
contained a verifiable signed catalogue.

Cause: external experiment summary, signed candidate catalogue and the
canonical root generated by YoloCanonicalArtifactBinding.ensure were
treated as the same input.

Fix: traced actual adapter/publication owners and executed catalogue signature,
catalogue schema, graph/weights hash and fixture/oracle reader checks on the
local package. All passed. Full adapter import separately failed because the
host wrapper lacks ndnsf._ndnsf; no native substitutes or inference were used.
Evidence: specs/183-tiger-yolo-reusable-experiments/evidence/yolo-input-validation.md.

Lesson: establish a missing artifact's actual consumer and schema before
classifying it as an external blocker. Preserve runtime and dispatch binding
requirements without inventing another signing authority.

# 2026-09-07 — Spec183 ranks could generate incompatible barrier identities

Symptom: run_rank without probe_id generated an independent random ID on each
rank; a two-rank invocation could not accept its peer's startup records.
Completion budget validation also happened only when its barrier was created.

Cause: a single-rank convenience default was reused for distributed startup,
and runtime-independent budget checks were delayed beyond runtime creation.

Fix: require a shared caller-provided probe ID for multiple ranks and validate
all three budgets before constructing NodeRuntime. Four targeted rejection
tests failed before the fix. The focused operator/startup suite now passes
16 tests, including real on-disk exchange between two rank barrier objects
(runtime and lifecycle mocked; no model or cluster qualification).

Lesson: invocation identity belongs to the coordinator, not individual ranks;
validate static budgets before creating resources.

# 2026-09-07 — Spec183 prepare could never reach its freezer

Symptom: a valid dispatch fixture passed the real content checks but prepare
always returned INCOMPLETE before freezing. After correcting that condition,
the same real CLI failed with HARNESS_DESTINATION_PARENT.

Cause: prepare demanded READY from check_operator_profile, an integrity-only
checker that always returns NOT_EVALUATED. READY-mocked tests hid this mismatch
and the missing run-root creation before freeze_harness.

Fix: separate offline byte freezing from execution qualification. Require
VERIFIED content, preserve NOT_EVALUATED and exit 78, reject a changed profile,
and exclusively create the run root. Local/submit/run execution gates remain
closed. The real CLI regression uses an audit hook prohibiting subprocesses
(except Python 3.8's uname -p import probe) and network connections; it verifies
the actual frozen bundle, refusal to run, and duplicate-run preservation.
Mutation cases reject a changed/unbound/writable source before output creation.

Lesson: test public commands without mocking qualification, and do not demand
runtime qualification merely to copy checked bytes for later qualification.

# 2026-09-07 — Spec183 preparation confused placement and runtime identity

Symptom: prepare_in_container used the catalogue placement digest for both
signed Provider offers and its preparation receipt, while workers expect the
enclosing prepared-run digest. With distinct real digests the later worker
boundary would reject preparation. Earlier fixtures reused one value.

Fix: the internal prepare-input-v2 descriptor and preparation API now require
explicit placement and runtime identities. Offers retain the placement digest;
receipt production and verification use the runtime digest. Reject ambiguous
v1 and missing/bad runtime identities. A producer test exercises both outputs
with mocked crypto/model owners; fake-container process tests reject receipt
substitution even after exit 0. No real cluster failure or runtime PASS claimed.

Lesson: fixtures must distinguish identities from different protocol layers;
reusing a single digest can hide a broken producer/consumer contract.

# 2026-09-07 — Spec183 operator profile lacked issuer input locators

Symptom: provision_run existed but its authority key, protection epoch and
public-input layout could not be derived from the concrete profile schema.
The prose contract mentioned private locators without defining their fields.

Fix: require the key locator and epoch explicitly; resolve the existing
template/package/registry references, runtime-plane SIF and catalogue candidate
into one v2 descriptor. Stage only rechecked small files and a 0600 key copy.
The installed issuer still owns signature/key matching. Eight synthetic-input
filesystem regressions cover mapping, private layout, stale bytes and rejection;
no production key or runtime launch occurred.

Lesson: a runtime function is not connected until every argument has a
documented, consumed source. Do not fill missing values with wrapper defaults.

# 2026-09-07 — Follow-up consumer missed during candidate identity split

Symptom: after separating issuer/runtime digests, run_requests still compared
offer.candidateDigest to worker._preparation_binding[2], which is a runtime
identity. The earlier producer-focused fix did not cover this consumer.

Fix: issuer verifies the selected catalogue ID/digest before issuing identities
and records both identities. Host provisioning checks both, and User compares
offer placement fields against its bound preparation receipt separately from
the worker runtime digest. Application fixtures now use distinct identities.
71 focused tests passed, including malformed/missing/swapped identity rejection.
No actual model or cluster execution was performed.

Lesson: identity-schema changes need a complete producer-to-consumer trace;
producer tests alone cannot establish that the runtime path is repaired.

# 2026-09-07 — Certified graph expectations lack a connected producer

Symptom: the final YOLO collector requires certifiedGraph, but the runtime
operator only forwards it. The comparator docstring claimed a preparation
producer existed; exact schema searches found only consumers and test data.

Cause: comparison tests supplied expectations directly, leaving provenance
and the actual production owner outside their coverage.

Action: corrected the inaccurate docstring and recorded the unresolved
producer/optimizer/publication binding tasks in Spec183. This is not a
runtime fix; T005/T006/T007 remain open and no coverage gate was weakened.

Lesson: every mandatory collector input needs a traceable production source;
synthetic expected data cannot establish an executable end-to-end workflow.

# 2026-09-07 — Final YOLO verdict demanded ORT execution from native Merge

Symptom: source-shaped final-component fixtures failed in all three normal
cases with FINAL_VERDICT_GRAPH_BINDING. Native Merge intentionally has no
ORT profile, but graph collection and final coverage demanded four ORT roles.

Cause: synthetic final fixtures reused the four-role execution inventory as
the ORT graph inventory, contradicting NativeYoloMergeRunner and the existing
native observation validator.

Fix: separate three-role ORT coverage from four-role execution/dependency
coverage. Preserve native Merge validation and the numerical oracle. Add
join/final regressions for absent or failed Merge, missing shard and invented
Merge ORT coverage. 153 focused tests passed; real inference is not claimed.

Lesson: different execution backends require different evidence, not fake
uniformity. A final fixture must follow actual producer semantics.

# 2026-09-07 — Base SIF digest intermittently mismatches under sandboxed reads

Symptom: dispatch-plane render/check and `sha256sum` intermittently reported
the 3.5 GB base SIF (`spec180-runtime.sif`) as `9008db7a…` while the recorded
identity is `b6710fd6…`; two consecutive `sha256sum` runs in one shell could
disagree, and render then check minutes apart disagreed.

Cause: the file itself never changed — metadata (mtime/ctime/size/nlink) was
frozen, no process held it, no twin copy existed, and repeated full reads
outside the sandbox plus warm-cache reads always returned `b6710fd6…`. The
failure appeared only for cold reads of the large file from sandboxed shell
commands.

Fix: run full-file digest checks and plane render/check for the base SIF
outside the sandbox (or retry once on `FILE_DIGEST`); the recorded identity
is stable and correct.

Lesson: a `FILE_DIGEST` rejection on an unchanged, single-writer CAS file can
be an environment read fault, not content corruption — confirm with a
non-sandboxed read before replacing any staged identity.

# 2026-09-07 — Dispatch plane parentId used raw plane.json file sha, not canonical id

Symptom: `spec183_dispatch_plane.py render` self-checks passed (parent_id
passed explicitly) yet an independent `check_chain` through dispatch failed
with PLANE_PARENT: runtime recomputed as eeb8afa0 while render reported
58c6f64f.

Cause: `_write_plane` returned the sha of the raw plane.json bytes and render
used that value as the stage id for the next plane's parentId. The stage id
`check_plane` returns (and every chain recomputes) is the canonical logical
document sha -- files reduced to `{bytes, sha256}` with no paths -- which
never equals the file bytes. The two are different identities by design.

Fix: render now derives every stage id from `check_plane(...)["id"]`;
`_write_plane` no longer returns a sha. Raw file shas remain correct for
profile `release` rows, which reference plane.json files.

Lesson: content-plane stage identity is the canonical document sha, not the
file sha; only release rows (file references) use file shas. Mixing the two
produces a chain that renders green and validates red.

# 2026-09-07 — Containerized offline issuer: three DI/runtime defects found by first real SIF execution

Symptom: the first genuinely executing Spec183 step (provision_run driving
`apps.yolo.py prepare` inside the base SIF) failed three times in sequence:
ImportError at ndnsf.runtime_telemetry (extension-less replay pythonWrapper),
ValueError YOLO catalogue signer not registered (spec180-signed catalogue vs
the Spec183 registry), and IDENTITY_REUSE:root (pre-existing .ndn under the
issuer HOME). A fourth failure (ModuleNotFoundError on py_repoclient's
compiled extension) was the same shadowing pattern as the first.

Cause:
1. The installed YOLO owner prepends the replay repo's pythonWrapper trees to
   sys.path (its launcher/child import-boundary rule).  The replay image ships
   pythonWrapper and py_repoclient WITHOUT their compiled extensions, so the
   extension-less copies shadowed site-packages and every `ndnsf.*` /
   `py_repoclient.*` import failed.  The owner's child PYTHONPATH puts
   site-packages first, so the SIF-internal execution path had never been
   exercised before Spec183.
2. The canonical package manifest's catalogue was signed by the Spec180
   authority (keyId spec180-...-20260903) while the Spec183 trust-root
   registry registers the fixed Spec183 catalogue key; the DI verifier also
   requires signature.authorityId, which the Spec183 wire format omits.
3. Importing the ndnsf extension initializes a default keychain under $HOME
   (`.ndn`) before issue() runs; issue() correctly treats that as identity
   reuse and fails.

Fix:
1. `_installed_yolo_owner` binds the installed `ndnsf` and `py_repoclient`
   packages in sys.modules before executing the owner, so all submodule
   imports resolve through the bound packages' __path__.
2. Re-signed the package catalogue with the fixed Spec183 catalogue key
   (candidates are the same; revision kept), including authorityId in the
   envelope; `sign_manifest` gained an optional authority_id parameter.  The
   re-signed package lives under .cache/model/spec183-signed/ (hard links for
   the model bytes).
3. issue() removes the root role's import-side-effect .ndn before its
   reuse check; every real role home is still checked unchanged.
Also fixed en route: `--home` must use the absolute container-path form
(host:container copies the image skeleton into the already-bound directory);
resolve_run_plan anchors the CLI --output to cwd, not the profile directory.

Lesson: a sealed runtime that no test ever executed hides owner-implemented
import-boundary bugs; the first real container execution is its own gate.
Fixing the scripts until the DI implementation works is exactly the Spec183
development path -- the SIF/DI itself needed no rebuild.

# 2026-09-07 — local-cpu rank: NFD/network real, controller blocked by stale in-SIF app

Symptom: the run-local development driver reached the Controller launch and
failed with CHILD_EXIT:controller:2: "unrecognized arguments:
--spec180-runtime-receipt-file". NFD, the forwarder config, and all four nfdc
network commands had already executed for real inside the SIF (exitCode 0).

Cause: the base SIF's in-image yolo_2x2 controller.py predates the Spec183
harness interface: the host checkout supports --spec180-runtime-receipt-file
(examples/.../controller.py:116) while the replay image copy does not.  The
base SIF is a Spec180-built input; the Spec183 harness is written against the
current host application sources.

Fix: none in-session -- the sealed SIF is not patched at runtime.  The
evidence (real NFD 24.07 startup, four successful nfdc commands, a written
tiger-yolo-network-setup-v1 receipt) stands; the application layer requires
the T011 local-SIF rebuild so in-image DI sources match the Spec183 harness
interface.  Recorded as a T011 precondition, not a harness workaround.

Lesson: a development driver can prove the infrastructure layer of a sealed
image immediately; application-layer interface drift between an old sealed
image and current harness sources is a rebuild trigger, not something to
paper over with runtime file injection.

# 2026-09-07 — T011 source handoff: revision cycle and cross-repo obstacles

Symptom: prepare-development-handoff rejected the checkout three ways in
sequence: HANDOFF_CHECKOUT_REVISION (frozen 20260906 lock pins
Experimental@447f7584 while the working branch carries the Spec183 fixes),
HANDOFF_SOURCE_UNTRACKED:examples/example-trust-anchor.cert (untracked
residue in the NAC-ABE checkout, not referenced by its build), and
HANDOFF_CHECKOUT_DIRTY on NDNSD (an uncommitted pkg-config Cflags fix).

Cause: the 20260906 lock is a frozen Spec180 input and must not be edited;
the sealer's clean-tree gate intentionally refuses dirty checkouts, and the
NAC-ABE file is not part of the built library.

Fix: generated a development-20260907 lock re-pinning all four repository
revisions to their current HEADs (old lock untouched); excluded the
untracked NAC-ABE example cert in the Spec183 sealer; committed the NDNSD
Cflags fix in its own repository (57d7431) and re-pinned it.  The handoff
bundle now seals SOURCE_READY (sourceSealDigest 2aea8a0e) and the
development definition renders (definitionSha256 c4f33beb).

Lesson: a frozen lock is an input identity, not a live pin; build-time
re-pinning is a new release with its own lock, and cross-repository dirt
must be resolved in the owning repository (or excluded when provably not
part of the build).

# 2026-09-07 — Spec183 progress obscured the controlling wiring gate

Symptom: tasks.md contained many incremental component-test checkpoints and
a later instruction to start T008, while the durable T007 audit remained
BLOCK. The host build evidence said RUNNING without a completed qualification
record. A reader could mistake recent activity for readiness or repeat broad
tests without closing the actual GPU YOLO invocation gaps.

Cause: parent checkboxes and historical prose did not expose concrete partial
steps, scoped evidence, blockers and the current next action together. Current
source still has launcher NOT_WIRED boundaries and no certified-graph producer
call in apps/yolo.py. The T008 driver is also incomplete: it does not build the
second Python extension, reuses build directories despite clearing the install
prefix, and suppresses the entrypoint failure with `|| true`.

Fix/remaining: added a maintained per-step table and dependency-ordered next
actions in tasks.md; updated the shared task template and installed local
Spec Kit generation/implementation/audit/convergence skills. The wiring and
build-driver defects remain explicitly BLOCKED work, not fixes claimed by this
documentation checkpoint. No build, model run or cluster test was launched;
pre-existing driver edits and the untracked host-unit record were preserved.

Tool evidence: Context Mode project health passed, but active health returned
exit 4 for stale tasks.md source hash; direct repository state was used as the
authority. GSD health was degraded (W017 old worktree, W019 noncanonical handoff);
Spec183 tasks.md and its audit were used instead of old phase state. Neither
warning justified deleting another worktree or restarting an experiment.
After the final task edit, file-backed authority was reindexed; project and
strict active health passed (five fresh active sources).

Skill validation: the stock skill-creator quick validator rejected the existing
Spec Kit `compatibility` frontmatter key. Preserved that unrelated metadata and
used a bounded YAML/name/description/progress-contract check for all four local
skills instead. The skills are local files excluded by `.git/info/exclude`;
the shared tracked task template carries the durable table contract. Table
validation also caught literal shell pipe characters splitting a Markdown row;
the cell was rewritten and all 31 substeps/17 parents/links then passed.

Lesson: distinguish implemented, component-verified and runtime-qualified
steps. Close the real wiring gap before formal qualification, reuse unchanged
evidence under the invalidation matrix, and do not turn table maintenance into
another full-suite or GPU campaign.

# 2026-09-07 — Spec183 ORT reference conflated logical and byte identities

Symptom: independent ORT reference preparation required the SHA-256 of
assembled ONNX bytes to equal the Selection artifact digest. Real YOLO roles
use a logical candidate/graph/role/node-cover digest, so a correctly assembled
model could not satisfy that check. Earlier tiny-model fixtures used the same
digest for both identities and missed the production incompatibility.

Cause: the reference boundary conflated the splitter's role contract with the
assembler's serialized output. Source tracing also showed that request-time
MODELROOT publication changes the model-manifest digest; an offline package
manifest cannot stand in for the final request binding.

Fix: require an explicit assembled_model_digest, verify actual bytes against
it, preserve artifact_digest for Selection comparison, and require the separate
assembledModelDigest in retained provenance. Added distinct-identity and digest
mutation checks; 145 affected tests passed in 5.52s. Evidence and command:
specs/183-tiger-yolo-reusable-experiments/evidence/t005-reference-identities.md.
The actual producer/request binding remains open; no runtime PASS is claimed.

Lesson: use source-derived identities in test fixtures and distinguish logical,
serialized and request-publication digests before wiring a distributed oracle.
Avoid broad reruns while its producer is still disconnected.

# 2026-09-07 — local operator omitted the prepared profile identity check

Symptom: a focused mutation supplied different current and prepared profile
digests; `_local` still tried to read the current profile's hostMinindn receipt.
The test failed at that forbidden boundary before any external command.

Cause: `_submit` and `_collect` compared profile identities, but `_local`
omitted the same check. Its currently disconnected worker prevented execution,
not the future cross-profile evidence mix once the worker is connected.

Fix: reject PROFILE_CHANGED_AFTER_PREPARE before reading the host receipt.
All 40 CLI tests passed in 33.80s; JUnit is retained under
Experiments/TigerCluster/results/spec183-local-profile-binding-20260907/.
The worker/staging wiring remains unfinished. No runtime qualification claimed.

Lesson: validate the same frozen candidate at every public execution boundary;
test mismatched identities before enabling expensive side effects.

Continuation tooling: project Context Mode health passed, while strict active
health reported NO_REAL_SESSION_EVENTS on this continuation. The repository
tasks/audit remained authoritative; no fabricated prompt marker or manual hook
fixture was used. Source edits were verified with CodeGraph sync and focused
tests. The live external build was polled by exact PID and left undisturbed.

# 2026-09-07 — per-request reference wiring exposed native import coupling

Symptom: real assembler reference test imported graph/plan through type-only
dependencies and reached an unusable host native extension. The generic native
application test independently failed collection with undefined NDNSD symbol
`_ZN5ndnsd9discovery16ServiceDiscoveryD1Ev` from the installed framework DSO.

Cause: offline assembler/graph values imported native-dependent classes eagerly;
the concurrently rebuilt host dependency closure is not yet qualified. Initial
schedule fixtures also omitted real runId/preflight fields newly required by
the connected User launcher, and compared tuple/list representations instead
of the canonical recipe digest.

Fix: defer annotation-only imports; retain a real local runtime import where
graph edges become InferenceDependency objects. Update explicit boundary
fixtures with run identity and declared preflight stand-ins. Compare canonical
recipe digests. The connected component selection passed 176 tests in 18.51s;
the native application check remains blocked, not skipped-as-PASS. Retain both
failed and passing JUnit files under results/spec183-request-reference-wiring-20260907.
The other client's build was not modified, restarted or claimed as qualification.

Lesson: test actual assembly bytes before runtime qualification; do not let
annotation imports force offline checks through stale DSOs. Reuse the same
prepared reference for identity mutations and rerun native checks only after
the loader closure changes. See evidence/t005-request-reference-wiring.md.

# 2026-09-07 — local dispatch waited for an impossible READY result

Symptom: local/collect required qualification=READY from check_operator_profile,
whose contract always returns NOT_EVALUATED. Future valid host receipts could
never reach local execution. Collection also depended on an undeclared host DI
package instead of the frozen NumPy oracle owner.

Cause: content integrity and prerequisite qualification were conflated; the
public local owner remained a placeholder while the lower owners were wired.

Fix: require VERIFIED integrity, consume the existing source-bound host gate,
check the matching nine-artifact native manifest, then execute/reanalyze from
the frozen CLI. Connect existing provision/rank/collection owners and snapshot
the canonical NumPy reference source in the explicit harness inventory. No
native/application/GPU PASS is inferred from these source connections.

Validation exposed two fixture/source issues: Tiger/lib is a pre-existing
compatibility symlink (map its declared canonical validator source explicitly,
do not relax frozen-bundle symlink rejection), and the prepare test returned
None where the actual adapter now supplies graph/catalogue provenance. After
the latter fixture correction only its 12 tests were rerun (0.33s), preserving
the initial 113-pass/1-failure JUnit. Latest per-case evidence covers all 114
unique selected components. Detail: Spec183 evidence/t004-local-owner-wiring.md.

Lesson: check documented return states through the actual caller. Freeze every
operator-side executable owner and verify source/gate identity before enabling
expensive effects. Correct narrow fixture failures without repeating model or
whole-suite campaigns. The other client's active host build was left intact.
The final import audit caught the remaining tensor decoder dependency before
qualification; its existing NumPy-only source is now frozen too. A fresh
isolated interpreter forbids all ndnsf/py_repoclient imports while loading both
owners, preventing test-suite module state from concealing a missing dependency.

## 2026-09-07 — Spec183 cross-run prerequisite identity and CLI fixture drift

Symptom: submit required READY from a checker that only reports content integrity;
generic prerequisite PASS did not bind prior run content. A run-specific candidate
also cannot establish content equivalence across different run paths.
Fix: v2 prepared receipts bind I/R/E; typed prior case and matching harness/global
behavior are required, followed by retained-data reanalysis against saved bindings.
Runtime candidate and numerical reference hashes are checked during collection.
Remote staging/run remain unavailable until their actual owners are connected.

Verification initially returned 48 passes / 3 fixture failures: two READY doubles
were stale, and a frozen empty CLI returned zero without executing validation.
New gate fixtures also omitted the renderer's parent directory (16 setup errors).
Corrected fixtures use actual frozen CLI source and an explicit parent directory;
56 targeted checks pass (20.60s), evidence in t004-gate-reuse.md. Lesson: test real
executable entrypoints at command boundaries, and do not mistake content validity
or a fake script's zero exit for runtime qualification. No GPU rerun was needed.

## 2026-09-07 — Spec183 effective snapshot lagged behind refreshed file rows

Symptom: a valid dispatch hash could describe old harness/contract settings.
Root cause: renderer generated effective-profile before _sync_profile_rows;
checker compared bytes but never the snapshot's actual settings with the profile.
Fix: shared canonical behavior owner, non-release rows refreshed before snapshot,
release rows last, and strict semantic equality at dispatch validation. Rehashed
wrong/extra fields are rejected before prepare. 79 focused components passed in
19.82s, including real small-plane render convergence and repeat stability;
evidence/t004-effective-profile.md records the exact command. Initial test import
order caused collection failure (runtime path unavailable); fixed the fixture
import order and retained the failed JUnit. Lesson: hash validity needs a binding
to the intended semantic object, and generators must prove one-pass convergence.

## 2026-09-07 — Spec183 batch runner had no path to the GPU worker

Symptom: the private batch action always raised RUNNER_NOT_WIRED despite the
existing allocation validator, CUDA probe and normal node lifecycle.
Fix: bind the single-node GPU batch/task to shared prepared paths and the E/case
journal, dispatch a finite srun task, and reuse the complete single-node owner
with an actual Slurm capture before native preparation. Retain srun cleanup and
require clean reaping plus matching collection job identity before reanalysis.
Do not release the journal from inside a still-running batch allocation.
63 focused tests then 11 final boundary tests passed; no runtime GPU execution.
Lesson: a clean worker receipt is insufficient for its outer launch process;
verify both, and keep task exit, numerical verdict and allocation termination
separate. Remote staging/scratch/terminal observer and distributed cases remain
unfinished; see evidence/t004-single-gpu-runner.md. No production gate was closed.

## 2026-09-07 — Real prepared plan could not reach collection handoff

Symptom: actual prepared plans omit candidateDigest, but the handoff writer
accessed plan.candidateDigest. Old fixture plans supplied a fictional field,
and composition tests doubled the final handoff, concealing the KeyError.
Fix: use the explicit bound runtime candidate argument for receipt and handoff
identity; test the actual plan shape and real retained receipt/file writer join.
The two-node owner also revealed a 630-second minimum with the full permission
windows; update the actual profile's insufficient 600-second walltime to 900,
preserving 1 warmup + 3 measured requests and their original deadlines.
41 focused owner/handoff and 53 CLI checks passed; no runtime tests were repeated.
Lesson: test adjacent owners with production-shaped data, and budget the full
invocation including permission acquisition. See t004-two-node-runner.md.

## 2026-09-07 — Declared storage budget was passed as measured capacity

Symptom: normal owners supplied peakBytes + marginBytes as repo_free_bytes;
GPU NFD node directories also lived under shared output, without allocated
local SIF staging. Those values could not establish real local storage readiness.
Fix: introduce the canonical allocated NodeScratch owner, measure statvfs bytes,
copy and verify the same SIF, probe fsync/Unix sockets, and retain final cleanup
records outside scratch. Copy and issuer share one bounded staging window.
Initial focused tests had three stale dictionary-identity assertions when the
GPU runtime mapping was copied to replace its SIF path; assert the passed path
and preserved descriptor instead. Final focused groups passed 56 and 66 checks.
Lesson: requested capacity is not observed capacity; statvfs is still not quota
or reservation proof. Distinguish physical-path relocation from content identity,
and never infer real GPU qualification from doubled launchers. No model reruns.

## 2026-09-07 — Storage checkpoint cannot refresh the pinned base SIF

Symptom: dispatch render and one bounded retry both fail FILE_DIGEST:baseSif.
An independent 1 MiB streaming read returns 6a3d001088305a9e189c7e97fe1ed19c8167347341de1ca23a1e67076f564b94,
not pinned b6710fd696a7f962f67f67a54278d92a15babb856c4af428a3f04ba54dc83285.
Size/mtime/ctime remain stable within that read; this does not prove correct bytes.
Root cause: unresolved; the earlier sandbox/cold-read diagnosis is not established
for this occurrence. Containment: preserve the lock/hash and failure, stop blind
retries, do not publish the new 25-file harness as a verified candidate. Next
compare trusted retained image evidence and host read integrity before repairing
or replacing input. Source/component storage checks remain separately recorded.
Lesson: do not normalize a surprising hash into a release identity or treat stable
metadata as a substitute for content verification. See t004-node-storage.md.

## 2026-09-07 — Base-SIF recovery verifies once, then changes observed digest

Remote retained input still hashes to pinned b6710fd6. Local original repeatedly
returned 6a3d0010. Stop the confirmed slow scp and recover to a distinct directory
with rsync delta transfer: only 59,376 literal bytes, original untouched. Recovered
file initially hashes b6710fd6; cmp finds a differing byte at 1,092,472,020.
The guarded replacement rehash then fails BEFORE changing any canonical link.
Later the reported differing window reads identically from both files, and a
single full recovery read gives c01cbda1 with both OpenSSL and independent _sha256.
Metadata stays stable during that read. No matching recent kernel errors found.
Root cause remains unresolved host/storage/read-path behavior, not proven stable
file corruption or an OpenSSL-specific bug. Preserve both files and original
locked identities; do not repeatedly download or promote the recovered file.
Exact evidence: specs/183-tiger-yolo-reusable-experiments/evidence/input-read-integrity.md.

## 2026-09-07 — Finished allocation could not close the Spec183 journal

Symptom: private batch correctly left its journal RUNNING but no external owner
verified termination and closed it, preventing the next independent allocation.
Fix: explicit collect --reconcile queries bound accounting plus the live queue,
retains the observation before journal closure and keeps default collect offline.
GPU prerequisite reuse also requires successful terminal evidence. Empty/failed
queries never release a job or resubmit. Scheduler errors remain separate from
model failures; a timeout leaves the existing numeric verdict and journal intact.
78 focused checks passed. Four new GPU-gate cases passed; a new frozen-entry
fixture lacked harnessManifestSha256, then passed after that required field was
added. No models or Slurm jobs ran. Unknown-submission recovery remains pending.

## 2026-09-07 — Submit had no receiver and the batch interpreter lacks dependencies

Symptom: even valid pre-staged normal runs ended in REMOTE_STAGING_NOT_WIRED;
unknown-submission recovery had no query owner. Separately, read-only iTiger
/usr/bin/python3 import fails immediately with ModuleNotFoundError: jsonschema.
Using a different local Python environment would not qualify the fixed batch
interpreter and would merely postpone this error until after sbatch.
Fix: wire the shared-path receiver with exact argv/intent, atomic SUBMITTING,
one no-requeue sbatch and bound acknowledgment; retry unknown submissions through
sacct/squeue only. Validate wrapper mode before external calls or reservation.
Add an actual batch interpreter/pinned dependency check before sbatch. The missing
site dependency is NOT repaired by these source changes; a maintained environment
and portable transport still need implementation. 85 initial component checks
and affected receiver regressions pass; see t004-shared-submit.md. No Slurm jobs,
model execution, SIF rebuild or download. The base-SIF read fault remains open.

## 2026-09-07 — Fixed batch Python bypassed a usable isolated environment

Symptom: Tiger's system Python lacks jsonschema. Installing a separate venv alone
would not repair hardcoded interpreter paths in pre-submit checks, the batch
wrapper and srun. Selecting an interpreter from mutable profile contents inside
the shell would also act before the frozen profile binding is checked.
Fix: create/reuse a pinned binary-only operator environment under project storage;
configure runtime.operatorPython; bind it as the sixth batch argument and use it
for srun and cluster frozen entry. Local CPU/offline collection use the invoking
host interpreter; frozen entry verifies its dependency pins. The wrapper never
chooses an executable by parsing mutable profile data. Raw profile digests remain
bound even though physical interpreter locations are excluded from behavior IDs.
Actual Tiger installation and reuse passed; local dependency pins also passed.
104 initial and 78 final focused checks passed, with overlapping groups reported
separately. No Slurm/SIF/GPU execution or host/native library replacement occurred.
Lesson: verify the exact interpreter at every relevant boundary and reuse a valid
environment instead of installing packages repeatedly. See t004-operator-env.md.

## 2026-09-07 — SSH transport omitted frozen directory permissions

Source review found that the new bootstrap and ordinary file receiver created
writable harness directories. Even correct file hashes/modes would therefore
fail verify_harness before the runtime entry. Fix: seal bootstrap directories;
after exact manifest/content checks, seal received harness directories under the
same publication lock. Already sealed, matching retries remain read-only so an
active unknown submission can reach its query owner. Real receive/seal/retry and
Tiger login-node transport now pass; no runtime qualification is inferred.

The first new test run had 14 setup errors and one pass because profile/prepare
fixtures inherited group-writable modes rejected by the declared transport
contract. Set explicit fixture modes; the subsequent 15 checks pass. Preserve
first.xml/log rather than hiding that failure. Real local rsync prefix resume
also verifies that --perms --chmod=F600 keeps staged payloads owner-only/writable.
Lesson: immutable content includes directory behavior at the next real consumer,
and fixture permissions must be explicit rather than depend on the host umask.
Evidence: specs/183-tiger-yolo-reusable-experiments/evidence/t004-ssh-coordinator.md.

## 2026-09-07 — Standalone native probe omitted original build closure

The new exact-output suppression probe initially omitted NAC_ABE_CMAKE_BUILD
and the framework include paths used by waf. After compilation, linking only
libndn-service-framework also omitted DI objects that tests/wscript normally
links separately. An unnecessary -lzstd flag and an assumption that ndn-cxx lived
under the clean prefix caused further command-level failures. Fix: reconstruct
the scoped build from actual waf settings, reuse stable unchanged DI objects in
a private archive, and verify the real DSO paths/hashes. All three new native
cases then passed; original logs are retained under the dependency-cutpoint
result root. The deployed native executable remains unqualified by this probe.
Lesson: derive the full compile/link closure before inventing a standalone test
command; the framework DSO is not the DI runtime archive. Keep component build
errors distinct from actual model/runtime failures. See t004-dependency-cutpoint.md.

## 2026-09-07 — Negative User schedule fixture used the wrong invocation-ID type

Two newly added schedule checks expected integer 0, but the existing User process
boundary deliberately receives string "0". Corrected only the fixture assertion;
the two targeted reruns pass. The composition check already passed and was not
repeated. Original output is retained with the negative User evidence. Lesson:
distinguish the integer request index in the prepared plan from the string
invocation identifier at the worker API; do not change production behavior to
accommodate a test assumption. See t004-negative-user.md.

## 2026-09-08 — Negative collector lacked production evidence and JSON-stable reanalysis

The old negative collection handoff accepted a caller-authored rejection record;
the actual two-rank negative owner/collector remained disconnected. Source review
also found that dependency tracing did not enable NativeProviderHandler's failure
log, so the required exact consumer error would be absent. Fix: reuse the normal
rank/cleanup owners, enable runtime timing for negative Providers, and derive the
rejection from bound User/Selection/cutpoint/consumer/native-GPU/node evidence.
Ordinary timeout and raw rejection records remain insufficient.

Two further semantic issues were found during the same join review: the helper
rejected even a protocol failure response although V17 requires zero successful
responses; and integer rank keys changed to strings on JSON persistence, making
unchanged offline reanalysis unequal. Accept a failed response only with all
independent fault evidence, and normalize JSON at the public collection boundary.
Targeted response/immutable-verdict checks pass. This does not qualify a real run.

The first new reader suite had 22 passes and one rank1 fixture failure: it reused
the CPU fixture's nfd0 launch instead of nfd1. Corrected the fixture service/log
identity, then the affected 191-test wiring group passed. Preserve that first log.
Lesson: trace actual producer-to-consumer evidence and serialized representations;
do not manufacture verdict facts, confuse failed responses with successes, or
infer rank layouts from a single-node fixture. Evidence: t004-negative-collection.md.

## 2026-09-08 — Host qualification boundary and stale audit blocked convergence

T007 source re-audit found that the Spec183 host receipt validator checks retained
file hashes but not their result semantics, returns COMPONENT_ONLY, and is still
consumed as the builder/local host prerequisite. The existing positive fixture
contains only case/kind records. The MiniNDN wrapper cannot yet produce the three
registered cases or this manifest, uses fixed shared paths/unverified preparation,
and lacks an outer process deadline. Separately, the YOLO issuer/rank path copies
apptainerVersion without enforcing it; the legacy worker's check is not on this
path. These are source findings, not observed forged qualification or GPU failure.

Root cause: component-boundary checks and later-stage wrapper drafts were not
reviewed together at their actual consumers. The old audit also continued to name
already-wired graph/negative owners and future T010/T011 results as current source
blockers. Resolution this checkpoint: correct the audit, plan and detailed progress
table; preserve BLOCK on explicit N1–N3. Code repair remains open: one real host
producer/semantic validator, a verified bounded three-case MiniNDN wrapper, and
issuer/rank version enforcement. Do not change qualification tokens as a shortcut.

Lesson: integrity is not result qualification; review downstream validation tools
without requiring their future physical PASS before the source audit can close.
Reuse existing component evidence and add only affected boundary checks. No test,
native build, model, SIF hash/transfer or GPU campaign was repeated for this audit.
Evidence: specs/183-tiger-yolo-reusable-experiments/evidence/design-code-convergence.md.
