# B189-4 Resource and Drain Evidence

**Status**: IN_PROGRESS / T008

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
