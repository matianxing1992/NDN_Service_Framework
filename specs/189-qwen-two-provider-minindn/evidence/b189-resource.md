# B189-4 Historical Resource and Drain Evidence (cross-cutting gate)

**Status**: IN_PROGRESS / cross-cutting gate owned by T003/T006/T009

T008 的历史文件名和证据链接保留，以免破坏旧记录；它不再是活动能力任务。新的
native counters 随实际 owner 交付，完整模型采样和 drain 只在 T009 的真实运行中
收口。已有 host guard/lifecycle/identity 结果仍只证明其各自边界，不能单独产生
`QWEN_TWO_PROVIDER_PASS`。

## Canonical identity bounded regression — r13, 2026-09-18

The repaired identity path was run against the real Qwen graph and its
1,503,264,768-byte external initializer through an immutable hardlink view
(the sidecar was not duplicated). It emitted
`SPEC189_CANONICAL_IDENTITY_PASS` for 311 tensors. `/usr/bin/time -v` recorded
3,689,700 kB peak RSS, 7.37 seconds elapsed, and `Swaps: 0`; the graph digest
was `sha256:0f3f6982c069b16d3bb1166f8d2ea6648c89419496869d01e9c1f81020ae0ca2`
and the normalized initializer digest was
`sha256:617db90e3f0cbc21fab2fac12855c626e459aef0bab1ce3002deaeab74c91756`.
Raw output is under
`.codex-tmp/spec189-qwen-two-provider-20260918/identity-r1/`.
This closes the focused identity/resource subunit; it does not qualify the
full MiniNDN request or prove the complete native cleanup path.

## Full-candidate preflight — r27, 2026-09-18

The global candidate entered the real MiniNDN launcher, but the host guard
stopped it during canonical source identity calculation before the requester
process was launched. The canonical graph protobuf is small; its external
initializer is 1,503,264,768 bytes. The old eager ONNX load reached about
4,234 MiB RSS and crossed the configured 256 MiB swap-I/O delta limit.

| Metric | Observed | Gate |
| --- | ---: | --- |
| `MemAvailable` at stop | about 3.65 GiB | minimum 1.5 GiB (not the first crossed limit) |
| process RSS peak | about 4.2 GiB | sampled by host guard |
| swap-I/O delta | 281,194,496 bytes | maximum 268,435,456 bytes |
| disk free | about 31.9 GiB | minimum 4 GiB |
| child cleanup | no remaining processes | `PASS` |

Supervisor result is `RESOURCE_BOUNDARY:swapIo`, return code `-15`, with raw
samples and receipt in
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-global-r27/workload/`.
This is a valid safety-stop diagnostic, not a protocol failure or qualification
result. The repair gate is the maintained ONNX identity helper: it must avoid
whole-model external-data materialization while preserving the canonical
digest, then pass a focused regression before another full candidate attempt.

## Direct entry and native lifecycle — 2026-09-18 14:11 -0500

**Baseline**: `638268bc` 加既有未提交 Native_Minindn 实现；只提交本轮增量，
不将其余既有实现混入 checkpoint。直接入口在 root/model 检查及模型物化之前运行
同一 supervisor；run root 0700，receipt exclusive 0600，冲突明确失败并保留旧证据。
LocalExperiment 传入同一有效 resourceLimits。内部 worker 无用户 CLI 绕过开关。

**Review trace**: 同一官方只读 agent，技能路径/hash 同下节。初审发现入口证据目录
权限与旧 receipt 冲突分类，修复并增加反例；v2 完整组合 STATIC_PASS。
冻结目录 `.codex-tmp/spec189-t008-direct-review-v2/`：native.patch SHA-256
`3f9ffad2758cafc7f2982ad42c5827a02ad046db0c90f76c725a473c9fcc3f06`，
caller-test.patch `afe1407bd826cf1eca3939e6166bad3faa5ade1773df2f5393943e3cce0ae886`。
三份改动源码均 cmp 与测试候选一致。native.patch 基于原有脏文件快照，非整个 HEAD diff。
五 lane：direct/main+LocalExperiment caller covered；runpy worker/同一 guard covered；
实际 CLI admission/权限/conflict 反例 covered；Python closure+现有 native targets covered；
receipt 身份/不覆盖及原有脏改动隔离 covered。原生计数采样仍 gap。

**Validation**: 同下节两个 pytest 文件共 39 passed / 6.63 秒。
在 `build-spec189-b189-3-global-r3` 内，以 system-first PATH、`/usr/bin/python3 ../waf
build --targets=spec185-runtime,spec185-provider-assembly -j4` 完成构建，2m27.504s。
复用全局依赖及现有构建树；Provider selector 首次编译其既有完整 DI source closure，
没有另建全树。g++ 9.4.0 / ld 2.34；监测初始少量 swap-out，后续无持续 swap-in/out。

每个 selector 连续运行三次（外层 timeout 45s），均成功：

- `spec185-runtime --run_test=Spec185Runtime/RuntimeCloseDrainAndAsyncNotificationAreSafe:Spec185Runtime/RuntimeDrainAsyncDoesNotImplicitlyClose`：每次 2 cases / 53 assertions。
- `spec185-provider-assembly --run_test=Spec185ProviderAssembly/Spec188ProviderReferenceAssembly/ProviderArtifactCacheStop*`：每次 2 cases / 9 assertions。

binary SHA-256：runtime `032ce99298f7235fbbfafcdd8e76a8d710c2f572ed58af25585f1a31a1e8797a`；
provider `c6fe60dc44324cbbf7a68e6b20d79174e0c6777a59cdf18a3f14b1ce7d14d535`。
原始日志 `.codex-tmp/spec189-t008-direct-validation-r1/`，包括 build、pytest、
vmstat、runtime-1..3 和 provider-1..3。只证明对应 C++ owner/drain 组件，不证明
真实 MiniNDN 全部进程的 Repo/materialization/runner 回收或完整 Qwen 成功。

**Four miss classes**: static=上述权限/conflict 两项已闭合；compile-link=成功，
保留既有 NativeRequestPlanner missing-field-initializers 警告，不据此认定行为失败；
runtime-test=39 host checks 和 4 native cases × 3 均 PASS；
unobserved=真实 producer/consumer native counters、模型峰值及完整实验。
**Batch growth decision / Closure decision**: direct guard 与小 lifecycle 出口关闭；
T008 仍 PARTIAL，下一步可按 T003 的小 fixture 接通 Repo producer/consumer，
并把已有 counter owner 接入真实路径；full-model 仍必须补齐 T008 全部要求。

磁盘：Git 报告临时 pack garbage 42.35 GiB；确认无 live pack/gc 后仅删除
48 个超过 24 小时的 `tmp_pack_*`（32.52 GiB）。正式 pack/index、模型和原始日志保留。
可用空间恢复约 34 GiB，`git fsck --connectivity-only --no-dangling` exit 0。
清单与检查日志 `.codex-tmp/spec189-t008-disk-cleanup/`；不执行 Git prune 或历史重写。

## Host supervisor validation — 2026-09-18 13:57 -0500

**Baseline**: `2424c24c`；本节取代下方 host guard 未开始的历史状态，T008 整体仍 PARTIAL。
维护 LocalExperiment 现用独立 Linux subreaper 执行 admission、持续采样与有界停止。
子进程 setsid/double-fork 仍归属本 supervisor；采样/正常清理异常走独立 kernel-child
回收，并保留 UNOBSERVED，不能把未知状态改成成功。资源阈值与 helper hash 进入候选身份。

**Review trace**: 唯一只读 agent `/root/spec185_review` 使用官方
`/home/tianxing/.codex/skills/review-agent/SKILL.md`，SHA-256
`07079efd0dc76f05fade424e5dfb048dce1de2df7626e1a4f56292a4f3f92228`。
v1 找到孤儿逃逸/证据冲突，v2 找到 sampler 异常清理及错误分类缺口；v3 复审和组合
审查 STATIC_PASS。冻结 `.codex-tmp/spec189-t008-review-v3/diff.patch` SHA-256
`5c12d39308128929e3fc812f4a4c802215fabd0f2fc72670a579977a9a3afe8f`；
helper、caller、测试三文件均 cmp 与实际测试源码一致。没有修改待审快照。

| Lane | Scope / result |
| --- | --- |
| production/callers | LocalExperiment → run_guarded 已接线；直接 Native_Minindn 入口待补 |
| implementation/wire | Linux subreaper、受控阈值、信号升级、紧急回收、分类已审查/测试 |
| test/harness/oracle | 实际小进程、double-fork/setsid、无关 child、采样异常、SIGTERM；仅 host assertions |
| build/source closure | Python 标准库 helper；C++ compile/link N/A；caller 绑定 helper hash |
| migration/evidence | profile 默认兼容与候选失效、旧证据不覆盖；native counters 明确 null |

**Validation**: `/usr/bin/python3 -m pytest -q tests/python/test_spec189_resource_guard.py
tests/python/test_spec184_qwen06b_local_experiment.py`：37 passed，6.51 秒。
原始日志 `.codex-tmp/spec189-t008-host-validation-r1/pytest.log`。
没有 native build、模型或 MiniNDN 运行，不授予 native/lifecycle/full-path PASS。

**Four miss classes**: static=上述两轮 controlling findings 均修复并复审；
compile-link=N/A；runtime-test=本轮 host tests 无失败；unobserved=直接入口保护、
原生 Repo/materialization/lease/runner counters 与 C++ drain，以及完整模型资源峰值。
**Batch growth decision**: host 独立出口已形成即测试；不加入模型接线职责。
**Closure decision**: host subunit CLOSED_FOR_VALIDATION；T008 OPEN_FOR_NEXT_BATCH，
下一出口为直接入口和既有原生生命周期 selectors，完整峰值仍由 T009 验证。

容量维护：使用 `uv cache clean`/`python3 -m pip cache purge` 清除可再生成包缓存；
工具报告约 965 MiB 缓存条目，因共享文件等因素磁盘实际可用仅约 1.4 GiB。
未删除模型、构建树或原始证据，full-model admission 尚不满足默认 4 GiB 下限。
Context active 索引在文档变化后曾 stale，已重建且 health 通过；源码/证据仍以仓库为准。

## Design binding — host supervisor

Direct-entry extension: `Native_Minindn.main(argv=None, *, _supervised=False)` 在
模型读取前调用同一 run_guarded，内部 worker 使用 Python runpy 调用，非新增用户 CLI
绕过开关。新增 `--resource-limits-json` 承接外层 profile 同一阈值；直接入口默认值
保持一致。supervision 单独记录且不得覆盖 child 的原生运行记录。外层 launcher
保留其 run-level guard，内层负责直接调用安全；二者停止均只处理各自后代。
新增 host CLI 测试以缺失模型路径和必定失败的受控资源阈值证明 admission 先于模型读取，
不启动 root MiniNDN。原生 lifecycle 使用既有 C++ fixture 验证，仍不替代 counters。

Allocation: T008 首个可独立验证出口为维护 LocalExperiment→native launcher 的
进程保护；不是 C++ 生命周期或完整 T008 PASS。新增
`Experiments/native_resource_guard.py::run_guarded(command, *, cwd, stdout,
sample_path, limits)` 替代无界 subprocess.run，返回 returncode、boundary、cleanup。
输入 limits 由 profile resourceLimits 校验/归一化并进入 candidate identity；
默认值保持旧 profile 可读，但有效值显式记录。helper hash 也进入 candidate。

FIELD/FLOW：supervisor 持有唯一新 session child、观测到的 descendant start-time 身份、
JSONL sample stream 和 deadline；启动前 MemAvailable/disk 检查，运行期间每秒采样
RSS、swap I/O、disk 和进程状态。超阈值先 SIGINT、限时 SIGTERM/SIGKILL，正常退出
也核对剩余后代。绝不对无关进程 killall。记录失败而不升级为协议 PASS。
Python 测试仅覆盖外部进程/资源设施；native Repo/lease/runner counters 和 C++
drain 仍属于本 T008 剩余出口，不以 host process census 代替。

PO：真实小 child 验证启动前拒绝、运行期阈值触发、deadline、遗留 child 清理、
正常退出与错误采样；不启动 MiniNDN，不消耗真实模型空间。
现有 Tiger sampler 仅 nvidia-smi，Repo helper 仅读 MemAvailable，均不提供监督/停止机制，
因此复用 /proc 读取惯例而不复刻另一个实验入口。

No Spec189 resource-guard run has been started. Historical Spec184/188 Qwen runs remain historical resource boundaries and are not reused as this candidate's result.

The current Qwen runner only starts, waits for markers and terminates child
processes; it does not yet sample RSS, MemAvailable, swap, Repo resident bytes,
materialization bytes or process-group drain. The r08 stale 1.5-GB publication
residue is therefore a recorded failure boundary, not T008 resource evidence.

## Five-lane coverage

All lanes are `gap` pending C++ lease/runner counters, host sampling and deterministic process-group drain.

## Closure decision

`OPEN_FOR_NEXT_BATCH` with trigger: add the C++ ownership counters and the
maintained Python sampler/guard, then run it around a complete or classified
B189-3 execution. A cleanup `finally` block alone is insufficient.
