# R11-B8-G11 Native Provider Co-location Evidence — 2026-09-10

## Scope and decision

本批针对多机部署与 MiniNDN 小拓扑之间的一个具体伸缩风险：多个逻辑 role
在机器数较少时需要共用同一个 Provider。审查覆盖 native placement、plan sealing、
role-specific grant、projection、dependency group、provider handler，以及对应的
C++ unit fixture。结论为 **CLOSED_FOR_VALIDATION**，范围仅是 C++ placement/co-location
边界；不提升 R11-B8 或 Spec182 父任务状态。

基线为本地 checkpoint `6420b0b26ecde981c5e4b54b01f75ebd5b5917a`。本批未改变
Python 旧 planner 的历史 one-to-one 约束；它属于 T013 caller migration 的兼容边界，
不能作为 native placement authority。生产方向仍是 C++ authority，Python 仅作薄调用层。

## Hidden constraints found and repaired

静态检查发现以下约束会使“小于 role 数的 Provider 拓扑”在不同阶段出现不一致：

1. `NativeV3Placement`、`NativePreSplitFirstPlacement` 和
   `NativePlacementProposal::validate` 要求每个 role 使用不同 Provider，或只按单个
   Provider 的容量判断。现在 placement 优先跨 Provider 分布，资源不足时允许同一
   Provider；CPU 可共置，GPU 必须使用不同的 offer-scoped device，并按 Provider 累计
   显存检查；累计 reservation 溢出时 fail closed。
2. `NativePlanSealer::NativePlacementPlanCore::validate`、request preparation 和
   selection projection validation 拒绝重复 Provider。现在这些阶段按 role 保留身份，
   只拒绝重复 role、未知 role、错误 offer 或容量/设备冲突。
3. Sealer 和 `NativePlanProjectionBuilder` 原来通过 Provider 找“第一个 role”，
   会把共置 Provider 的后续 role 绑定到错误 grant。grant view、acquire 和 projection
   现在显式传递并校验 role identity；request planner 按 role 获取每个受保护 grant。
4. Group projection 原来按 role 产生重复 admitted offer，或拒绝只有一个 Provider 的
   dependency group。现在 group builder 按 Provider identity 去重 offer，同时保留每个
   role 的 projection，并允许容量约束内的一成员 protected group。
5. Qwen maintained profile 仍固定三阶段 role 顺序，这是模型适配器契约；仅放宽
   Provider identity 可重复，未放宽 role 数、role 名称、rank 或 tensor 语义约束。

## Review and verification matrix

| Lane | Evidence | Result |
| --- | --- | --- |
| Production entry/callers | CodeGraph/static trace from placement through sealer, grant client, request planner, projection and group builder; maintained caller migration remains open | `STATIC_PASS` for the changed native boundary; caller closure open |
| Implementation/wire | Explicit role identity in grants/projections; Provider offer deduplication; CPU co-location and GPU device/memory reservations | `STATIC_PASS` |
| Tests/harness | Added Qwen shared-Provider round trip, generic placement fallback, V3 distinct-device co-location, co-located group projection, duplicate-provider projection and sealer cases | `FOCUSED_BEHAVIOR_PASS` |
| Build/source closure | Fresh Waf `unit-tests` build, system-first PATH, `-j2`; elapsed 51.43 s, max RSS 1,657,040 KB, exit 0 | `BUILD_PASS` |
| Migration/evidence | `tasks.md`, `plan.md`, native-first contract and this record synchronized; Python legacy one-to-one, no-Python, maintained callers, MiniNDN and T016/T017 remain open | `PARTIAL` at parent level |

## Commands and results

The first rebuild after adding checked reservation arithmetic failed at the compile
boundary in `NativeV3Placement.cpp`: the new multi-statement reservation block was
placed under an existing single-line `if` without braces. No binary from that attempt
was used for behavior claims; the source was corrected before the passing rebuild below.

Fresh build:

```text
env PATH=/usr/bin:/bin:/usr/sbin:/sbin /usr/bin/time -f 'elapsed=%e maxrss=%M exit=%x' \
  ./waf -o .codex-tmp/spec182-r11-b2-fresh-20260910/build build \
  --targets=unit-tests -j2
elapsed=51.43 maxrss=1657040 exit=0
```

After the checked reservation arithmetic was added and the compile-boundary mistake was
corrected, the same target rebuilt the two changed translation units and relinked
successfully: `elapsed=18.03 maxrss=1134916 exit=0`.

The following C++ suites passed after the final source edits:

```text
Spec182V3Placement:              10 cases, 577 assertions
Spec182NativePlanning:           30 cases, 882 assertions
Spec182PlanSealer:               12 cases, 71 assertions
DiQwenGenerationSession:         17 cases, 97 assertions
Spec182*:                       258 cases, 7099 assertions (two consecutive runs after the final source patch)
```

One earlier cross-test run produced a non-reproducible SIGSEGV in an existing seeded
conversation case. The isolated case was rerun three times and the full `Spec182*` group
then passed twice; it is retained as a stability limitation, not counted as a qualification
failure or silently converted to PASS.

## Boundaries and next work

本批证明的是容量受限拓扑下 C++ placement-to-projection 的一致性。它没有证明真实
NDN 多机传输、完整模型推理、0.6B 模型、MiniNDN 全验证矩阵、no-Python runtime、
maintained caller migration、旧 Python runtime 退出或 T017 qualification。下一批应在
保持 role/Provider identity 契约的前提下，接通独立 C++ requester/Provider 的真实两轮
请求，再进入 stream/recovery、maintained callers 和 MiniNDN 资格门。

## Retrospective

本批静态门提前发现了跨层 one-role-per-Provider 假设；编译门捕获了测试 fixture 的
构造方式问题；运行门确认 co-location、role-specific grant 和 group projection 的
行为。仍未观察到的类别是跨进程 NDN 传输、真实模型资源占用、节点故障恢复和 Python
调用方迁移，因此本记录只关闭当前边界，不把局部测试数量解释为整体完成。
