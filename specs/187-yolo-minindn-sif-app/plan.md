# Implementation Plan: YOLO MiniNDN SIF+APP Fast Path

**Branch**: 187-yolo-minindn-sif-app | **Date**: 2026-09-15 | **Spec**: [spec.md](spec.md)

## Summary

2026-09-15 补充 B187-BASE：T001 的基础制品前置由 `build-base-sif.py` 负责。依据当前 SIF+APP 分层修复历史脚本，单独验证锁定父镜像、NumPy 私有库和镜像内 C++ SDK consumer；批次出口为 `BASE_SMOKE_ONLY`，不扩展为 APP 或 MiniNDN 资格。五 lane 和构建证据见 [base repair](evidence/b187-base-repair.md)。

本计划把用户需要的最快路径固定为一个 YOLO 垂直切片：复用已有 TigerCluster 构建和运行入口，先在本机用 MiniNDN 验证真实请求，再把同一不可变 pair 交付 TigerCluster。QWEN 不进入本轮实现或验收。

## Technical Context

**Language/Version**: C++ production and tests; Python 3 orchestration and binding checks

**Primary Dependencies**: Apptainer 1.5.3, MiniNDN/NFD, existing NDNSF Core/DI/SVS/Repo scripts

**Storage**: sealed source bundles, regular SIF, immutable APP directory, model/identity/NFD mounts and evidence files

**Testing**: registered C++ selector, focused Python script checks, MiniNDN local run, bounded TigerCluster run

**Target Platform**: local Linux host first, then TigerCluster Slurm compute node

**Project Type**: native distributed-inference runtime, container delivery and network experiment

**Performance Goals**: one documented local sequence; two repeatable local YOLO runs before one bounded cluster run

**Constraints**: no host-library fallback, no stale symlink inputs, no cluster build before local PASS, no QWEN dependency

**Scale/Scope**: one YOLO model/profile and one bounded MiniNDN topology; no QWEN streaming qualification

## Constitution Check

PASS with the following obligations: existing TigerCluster entrypoints remain the sole SIF implementation; native behavior is owned by a C++ selector; local validation precedes cluster work; candidate closure and invalidation are executable gates; implementation changes require design-to-code convergence before formal MiniNDN/SIF/Tiger evidence.

## Pre-Qualification Design-Code Convergence

**Design authority**: spec.md, contracts/yolo-pair.md, data-model.md, quickstart.md and the existing layered runtime contract in specs/182-native-di-python-bindings/contracts/layered-runtime-delivery.md.

**Production paths to inspect**: prepare-development-handoff.py, build-local-sif.sh, build-sif-app.py, validate-sif-app.py, run-sif-app.sh, Experiments/NDNSF_DI_YoloAckDriven_Minindn.py, tests/wscript and the C++ selector source.

**Required audit artifact**: evidence/convergence-<run-id>.md with a five-lane map, project-symbol definition map and severity-classified gaps.

**Closure rule**: every controlling semantic, security, wiring and evidence gap is repaired with a focused regression; a fresh audit reports PASS.

**Formal validation boundary**: local C++/MiniNDN runs, then SIF pair validation and the bounded TigerCluster job.

**Re-audit triggers**: source/ABI/dependency, definition/base/APP, model/profile/identity/mount, selector/harness or evidence-contract changes.

## Logical Batch Quality Plan

| Batch ID | Behavior boundary / stable exit | Members | Implementation dependencies | Acceptance dependencies | Coverage matrix scope | Shared build/test selector and owner | Risk class / Dynamic profile / invariants | Review trace / closure decision | Result record / evidence owner |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| B187-LOCAL-CLOSURE | 输入闭合并能生成不可变 pair manifest | T001 | existing scripts and base contract | regular base SIF and host-gate manifest | production callers, script wiring, tests, source closure, evidence; exact queries recorded in evidence | existing script tests plus closure mutation checks; main agent | high / none; reject stale/symlink/host fallback with zero side effects | review-agent snapshot required; OPEN_FOR_NEXT_BATCH until local pair is validated | evidence/b187-local-closure.md |
| B187-LOCAL-YOLO | 本机同一 pair 连续两次产生真实 YOLO terminal result | T002,T003 | B187-LOCAL-CLOSURE | convergence PASS and C++ selector linked | five lanes plus symbol map for selector target | Spec187YoloMiniNdn; main agent | high / asan-ubsan; ACK→Selection→Provider→Response and cleanup | composition review then CLOSED_FOR_VALIDATION only after two C++ runs | evidence/b187-local-yolo.md |
| B187-TIGER | 同一 pair 在 TigerCluster 完成一次有界运行 | T004 | B187-LOCAL-YOLO | LOCAL_PASS and unchanged pair identity | five lanes; scheduler/mount evidence separate | same selector through run-sif-app.sh; main agent | medium / none; digest/profile unchanged and no rebuild | OPEN_FOR_NEXT_BATCH until cluster evidence exists | evidence/b187-tiger-yolo.md |
| B187-DEFERRED | QWEN remains explicit TODO | T005 | — | documentation consistency | migration/evidence lane; other lanes N/A by scope | docs checks; main agent | none / none; no QWEN input read | CLOSED_FOR_VALIDATION after docs checks | evidence/qwen-deferred.md |

Each batch stops at its stable exit. A later caller, selector, source closure or hard acceptance dependency creates a new batch instead of expanding the current one. Four miss classes and comparable build scope/elapsed are recorded in the batch evidence.

## End-to-End Candidate Gate (mandatory before SIF packing)

The candidate is planned and checked as one closure, even though its work is
implemented in bounded units. The order is fixed:

1. **Source closure**: seal the exact source revision and enumerate every
   production header, shared library, executable, Python extension, pkg-config
   file and runtime manifest that the definition is expected to install.
2. **Container build**: compile Core/DI/Repo/UAV and bindings inside the base
   SIF. The builder must install the complete public header tree from that same
   source seal; an inherited header or library is a failure.
3. **Pre-pack consumer gate**: before creating a squashfs/SIF, run the SDK
   verifier and a real C++ consumer compile/link against the assembled runtime
   tree. Compare installed headers and loaded libraries with the source/build
   manifests. Any failure stops the batch before the expensive pack step.
4. **SIF gate**: create the SIF only from the verified tree, then run the same
   native verifier under `--cleanenv --containall` with all implicit host binds
   disabled. The SIF digest and labels become immutable candidate identity.
5. **Behavior gate**: run the container C++ unit selector, then the native YOLO
   smoke, then the through-MiniNDN request. Python only orchestrates these
   processes and records evidence; it cannot close a native behavior task.
6. **Promotion gate**: only the unchanged SIF plus its source/model/profile
   digests may be sent to TigerCluster. QWEN remains outside this closure.

The build receipt records each gate separately. A successful earlier gate is not
retroactively changed by a later failure, and no later gate may be used to infer
an unobserved earlier gate. This order is the primary rework control for the
remaining Spec187 work.

## Project Structure

### Documentation

specs/187-yolo-minindn-sif-app/ contains spec.md, plan.md, research.md, data-model.md, quickstart.md, contracts/yolo-pair.md, checklists/requirements.md and tasks.md.

### Source and experiment paths

Experiments/TigerCluster/adapters/slurm-apptainer/scripts/, Experiments/TigerCluster/adapters/slurm-apptainer/templates/, Experiments/NDNSF_DI_YoloAckDriven_Minindn.py, tests/integration-tests/, tests/standalone/ and tests/wscript.

**Structure Decision**: 复用上述现有目录；只在 Spec187 需要新的 C++ selector、最小调用方接线、验证脚本或文档时增量修改，不复制 TigerCluster 共享实现。

## Notes

2026-09-15 两层修订：优先在已验证 base 上补齐全部外部依赖/SDK，再由 NDNSF 层消费。B187-BASE-SDK 的稳定出口是新 base 的 C++/Rust/ELF/Python 验收；B187-NDNSF-CONSUMER 的稳定出口是无依赖安装/重编的仓库候选与本地 YOLO。两批分别静态门和构建验证；兼容 APP 字段不形成第三交付层，详见 [two-layer delivery](../../Experiments/TigerCluster/docs/two-layer-delivery.md)。

2026-09-15 本地构建顺序修正：允许通过现有 `build-local-sif.sh --build-only` 先生成 `BUILT_UNQUALIFIED` 候选，用于当前源码容器编译和本机测试。该模式仍要求源封存、容器原生边界、镜像标签身份及模板内验证；不执行或声称旧 Spec175 host/模型资格。发布路径的 host-gate 与 `PASS` 要求保持不变，既有 release validator / APP packager 必须拒绝 `BUILT_UNQUALIFIED`。T001/T003 仍须实际 APP 与请求链验收才能完成。本模式不授权 Tiger 上传或运行。

实现前置是取得 regular base SIF。当前本机 dangling link 只能产生 WAITING_EXTERNAL_INPUT，不能作为 build 或 runtime evidence。
