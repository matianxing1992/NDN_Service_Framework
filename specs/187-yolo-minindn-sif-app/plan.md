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

## Project Structure

### Documentation

specs/187-yolo-minindn-sif-app/ contains spec.md, plan.md, research.md, data-model.md, quickstart.md, contracts/yolo-pair.md, checklists/requirements.md and tasks.md.

### Source and experiment paths

Experiments/TigerCluster/adapters/slurm-apptainer/scripts/, Experiments/TigerCluster/adapters/slurm-apptainer/templates/, Experiments/NDNSF_DI_YoloAckDriven_Minindn.py, tests/integration-tests/, tests/standalone/ and tests/wscript.

**Structure Decision**: 复用上述现有目录；只在 Spec187 需要新的 C++ selector、最小调用方接线、验证脚本或文档时增量修改，不复制 TigerCluster 共享实现。

## Notes

实现前置是取得 regular base SIF。当前本机 dangling link 只能产生 WAITING_EXTERNAL_INPUT，不能作为 build 或 runtime evidence。
