# B1 Request Correctness Evidence

**Status**: PARTIAL / static gates complete, C++ batch validation pending
**Spec**: 184-native-di-closure
**Checkpoint**: `0947fc1b` plus the uncommitted B1 working diff

## Scope and batch decision

B1 保持两个相互依赖但可分别审查的任务：T001 负责 authority request 的 Face IO ownership，T002 负责 requester operation 的 conversation turn/attempt publication。两项都能在同一次 unit/integration build 中验证，暂不加入 durable outcome、checkpoint export 或 caller migration；批次边界稳定。

### Dynamic validation contract

- Risk class: `concurrency/lifetime`。
- Dynamic profile: `tsan`，使用独立 sanitizer build/output；T001 和 T002 共用 B1 profile，
  不为每个小任务重复构建。
- Invariants: `ServiceUser` pending-call mutation remains on the Face IO owner；turn/ticket
  publication and abort have one linearization；terminal/abandoned operations ignore late callbacks；
  pending-call and conversation-ticket residue returns to zero。
- Current result: `NOT_RUN`。静态审查和普通 C++ selector 结果不能升级为 `DYNAMIC_PASS`；TSan
  必须在 B1 普通定向行为测试完成后运行并保存报告、重复次数、exit code 和实际 binary digest。

## T001 static gate

- Production path: `NativeAuthenticatedGrantClient::coreIssue` → `ServiceUser::postToIo` → `ServiceUser::RequestServiceTargeted`.
- Worker remains the blocking wait owner. The Core IO closure owns admission, timeout registration, response publication, empty request-ID rejection, and dispatch exception propagation. The shared `Pending::abandoned` flag fences cancellation before dispatch and late callbacks.
- C++ selectors: `Spec184AuthorityIoOwnership` covers real `ServiceUser` IO-thread ownership, canonical authority wire, worker/IO thread separation, and pending-call cleanup. `Spec184AuthorityDispatchCancellationAndException` covers pre-dispatch cancellation and an IO-side malformed-name exception.
- Target closure: `tests/integration-tests/di-native-requester-grant.t.cpp` is explicitly registered by `tests/wscript`.

## T002 static gate

- Production path: `beginCoreRequest` ACK-closed worker snapshots `runtime`, request options, inspected model, encoded request, strategies, coordinator, and turn under `operation->mutex`; planner and role-map binding operate on local copies; `planned` and the bound turn publish together only when status, attempt, phase, and ticket identity still match.
- A stale terminal/replacement result aborts the local coordinator ticket after the lock is released. The Core commit closure receives a copied `CollaborationPlan`, so it does not read `operation->planned` without the operation lock.
- C++ selector: `Spec184TurnPublicationRace` deterministically races initial/replacement role-map publication against `abortTurn`, then verifies stale token acceptance is rejected. Existing Spec182 client lifecycle tests remain supplementary coverage for cancellation/deadline/close, while the R4-B6 production conversation integration remains the end-to-end replacement/receipt harness.
- Target closure: `tests/unit-tests/di-native-client.t.cpp` is included by the unit-test glob in `tests/wscript`.

## Official review trace

The read-only review followed `/home/tianxing/.codex/skills/review-agent/SKILL.md` (SHA-256 `07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`) against the complete B1 diff and surrounding production callers. The five lanes were checked: production entry/callers, implementation/wire, C++ tests/harness, target/source closure, and migration/evidence. `git diff --check` passed; no actionable P1/P2 finding remained after removing the unused dispatch flag and synchronizing the turn publication path.

## Validation boundary

The first build attempt did not start a compile: configuring with an external
`WAFLOCK=/tmp/spec184-b1-waf.lock` succeeded, but the subsequent build rejected
the output as not configured. This Waf lock/output boundary is recorded in
[`docs/failure-log.md`](../../../docs/failure-log.md) and raw files under
`.codex-tmp/spec184-b1/`; it is not a source or runtime result. T001 and T002
remain `[ ]` in `tasks.md` until a fresh system-toolchain `-j4` build and the
named C++ selectors pass together. Python wrapper, migration-document PASS, and
old Spec182 evidence cannot promote either task.

The corrected-lock retry compiled 289/309 tasks in 8:33 (`MAXRSS=2089168KB`)
before the new T001 test TU stopped on fixture namespace/type qualification
errors. The incremental retry then accepted the namespace correction but found
the remaining unqualified `ResponseMessage` and lambda overload mismatch. This
is a test-only compile boundary; no production source error was reported. The
exact outputs are retained in `build-retry2.log` and `build-retry3.log` and will
be reused after the final qualification correction. The following incremental
compile reached the same test TU and found a constness mismatch at
`ResponseMessage::setPayload`; that third test-only boundary is in
`build-retry4.log`.

## Miss and closure record

- Static lane: direct `RequestServiceTargeted` is now only inside the Core IO closure; all changed turn/attempt fields have lock-protected snapshots or publication checks.
- Compile/link lane: Waf configuration was repaired; the first compile reached the new test TU and stopped on test-only type/overload errors; retry pending.
- Runtime lane: pending the named unit/integration selectors, including bounded pending-call cleanup.
- Unobserved lane: no MiniNDN/Qwen/no-Python qualification is authorized before B4/B5.
