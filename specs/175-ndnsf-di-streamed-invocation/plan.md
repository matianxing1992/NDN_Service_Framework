# Implementation Plan: NDNSF-DI Streamed Invocation Local Closure

**Branch**: `Experimental` | **Date**: 2026-09-03 | **Spec**: [spec.md](spec.md)

**Revision 110 handoff rule (2026-09-03)**: This plan is frozen and has no
remaining implementation or experiment task. The shared-tree
`DIRTY_INPUT_TREE` result is expected after Spec180 edits and must not be
interpreted as a Spec175 failure. Current YOLO candidate closure, live
ACK/Selection/Response evidence, QWEN-F production objects, SIF, and
TigerCluster are receiving-feature work only. If one of those is missing,
record the Spec180 owner and stop at its gate rather than reopening this plan.

## Summary

Freeze the already-developed generic streamed-invocation and Qwen stateful
generation contracts, repair only discrepancies exposed by the frozen source
code-aware audit, then accept the implementation through complete local suites
and two representative CPU/MiniNDN production-path cases. Publish a precise
handoff to Spec180. No SIF or Tiger work remains in Spec175.

**Current execution state (revision 105)**: this plan is complete and frozen. It has no next
implementation step. Its `LOCAL_FUNCTIONAL_PASS` is bound to the named
Spec175 source seal only; it does not qualify the current dirty working tree.
Any failure in current source, the ACK-driven YOLO path,
the external Qwen3.6-27B path, SIF construction, or Tiger execution returns to
the corresponding Spec180 task; it must not create another Spec175 repair or
qualification loop.

The receiving feature now names its first two gates explicitly:
`G0-NATIVE` (one ABI-identical native closure) followed by
`G0-CANDIDATE` (trusted YOLO inputs). Neither gate reopens this plan.
The frozen local-build convention is `-j2`; Spec180 must retain it and may not
reinterpret the handoff as permission for higher compiler concurrency.

## Technical Context

**Language/Version**: C++17; Python runtime and bindings used by the current
host build; Bash/Python MiniNDN harness.

**Primary Dependencies**: ndn-cxx; NDN-SVS `Experimental` with Boost 1.71;
NAC-ABE; NFD; pybind11; ONNX Runtime CPU; standalone tokenizer support.

**Storage**: Bounded in-memory stream state; role-local generation state;
encrypted conversation journal; content-addressed tiny ONNX fixtures; JSON/JSONL
local evidence.

**Testing**: Boost.Test unit tests, C++ integration tests, pytest, deterministic
ONNX oracles, and host CPU/MiniNDN production-path tests.

**Target Platform**: Linux host and CPU MiniNDN only.

**Performance Goals**: None. Timing is diagnostic and cannot qualify Spec175.

**Constraints**: One Provider owns one complete role; one plan per request;
one terminal Response; no per-token service requests; no live state tensors on
NDN links; replacement remains disabled by default; no deployed
PyTorch/Transformers runtime.

## Constitution Check

- **Canonical dynamic runtime**: PASS; the feature extends the generic dynamic
  request lifecycle and preserves unified service names.
- **Security in the data path**: PASS by contract; the audit must verify the
  production path has no bypass.
- **CodeGraph first**: REQUIRED for the closure audit and any repair.
- **Spec-driven work**: PASS; this plan and its contracts are the accepted
  authority.
- **Right validation scope**: PASS; complete local suites and MiniNDN follow
  audit PASS, while expensive qualification moves to Spec180.
- **Cohesive tasks**: PASS; each task closes one reviewable gate.
- **Immutable promotion**: NOT APPLICABLE inside Spec175 because it performs no
  SIF build or remote action; Spec180 owns this obligation.

## Pre-Qualification Design-Code Convergence

**Design authority**: [spec.md](spec.md),
[requirements-v1.md](contracts/requirements-v1.md), the API/wire/generation/
epoch contracts, and the one-Provider/one-complete-role invariant.

**Production paths to inspect**: generic streaming APIs and wire handling in
`ndn-service-framework/` and `pythonWrapper/`; automatic planning and
conversation coordination in `NDNSF-DistributedInference/`; native Provider
execution, state, and ONNX owners; current MiniNDN launcher and evidence parser.

**Required audit artifact**: `audit.md`, containing requirement-to-owner-to-test
traceability and severity-classified gaps.

**Closure rule**: Every semantic, architecture, security, production-wiring,
or evidence-validity gap is repaired with a focused regression; a fresh audit
must return `PASS`.

**Handoff rule (revision 105)**: Once the named source seal is closed, the
active feature pointer remains on Spec180. Do not use a Spec175 rerun to
qualify a later source tree, candidate, model, or deployment image.

**Formal validation boundary**: The complete unit/integration suites and the
two registered MiniNDN cases cannot be accepted until the audit passes.

**Re-audit triggers**: Any behavior-affecting source, dependency, effective
configuration, test harness, protocol contract, or evidence-schema change.

## Execution Plan

### Phase A — Freeze and converge

Use CodeGraph and exact source inspection to map FR-001--FR-010 to the current
public entrypoints and production call paths. Repair only controlling gaps and
their focused regressions. Historical SIF/Tiger evidence cannot close a gap.

### Phase B — Complete local validation

From one recorded source identity, run the complete relevant native and Python
suites. Then execute two CPU/MiniNDN cases through real NFD/SVS/security and the
production Provider/User path:

1. cold streamed generation with deterministic ordered tokens and terminal
   Response;
2. two-turn same-conversation continuation plus the registered mismatch
   rejection.

Each case records request/attempt/plan lineage, all child exits, and cleanup.

### Phase C — Handoff

Write one closure record with `LOCAL_FUNCTIONAL_PASS` or
`LOCAL_UNQUALIFIED`. Freeze the APIs and invariants consumed by Spec180, list
the existing production owners, and explicitly transfer YOLO ACK-driven
integration plus all SIF/CUDA/Tiger validation.

## Project Structure

```text
specs/175-ndnsf-di-streamed-invocation/
├── spec.md
├── plan.md
├── tasks.md
├── contracts/
├── audit.md
├── traceability.md
├── handoff-to-spec180.md
└── evidence/

ndn-service-framework/                 # generic stream lifecycle and wire owners
pythonWrapper/                         # Python stream surface
NDNSF-DistributedInference/            # planning, Qwen, state, native execution
Experiments/                            # CPU/MiniNDN cases
tests/                                  # native, integration, and Python gates
```

**Structure Decision**: Keep model-neutral lifecycle/security in Core, Qwen
generation semantics in its adapter, Provider execution/state in the native DI
runtime, and topology/evidence policy in the MiniNDN harness.

## Migration and Rollback

- Unary behavior remains the rollback path; it must not allocate streamed state.
- Streamed behavior remains opt-in and must fail as unsupported rather than
  silently downgrade.
- Conversation continuation remains optional; disabling it retains
  request-scoped full-context generation.
- No SIF or remote artifact is promoted by Spec175, so rollback requires no
  cluster cleanup or remote migration.
