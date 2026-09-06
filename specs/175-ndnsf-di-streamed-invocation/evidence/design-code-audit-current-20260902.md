# Spec175 Current Design-to-Code Convergence Evidence

**Date**: 2026-09-02
**Subject**: current worktree source seal and Spec175 production paths
**Verdict**: `PASS` for the convergence gate; later local qualification remains
  open

## Authority and source identity

- Source revision: `286a0098b9bf0dfc2e0b77a320e9e75c38851dc7`
- Source seal: `evidence/t020-source-seal-current-20260902.json`
- G0 manifest: `evidence/t020-g0-current-20260902.json`
- G0 status: `PASS`, zero blockers; the seal covers 121 in-scope dirty source
  files.
- Context Mode active-feature health and CodeGraph index were current before
  this audit.

## Production-path inspection

| Requirement group | Production owner inspected | Focused evidence |
|---|---|---|
| FR-001--FR-004 | `ServiceUser::RequestServiceStreaming`, `ServiceProvider::addStreamingHandler`, Python `request_streaming`, and the existing Request/ACK/Selection owner | `test_spec175_streamed_generation.py`, `test_streamed_invocation_api.py`, G0 source census |
| FR-005--FR-007 | `NativeEpochCoordinator`, `NativeProviderHandler`, Qwen generation state machine, and conversation checkpoint owners | `test_spec175_conversation.py`, `test_spec175_qwen_generation.py`, native source markers in G0 |
| FR-008 | signed assignment and canonical object binding in `NativeProviderHandler`, `NativeCanonicalOnnxAssembler`, and executable launcher | `test_spec175_onnx_deployment_boundary.py`, canonical-source and fail-closed launcher markers in G0 |
| FR-009 | current-source gate, production MiniNDN launcher, child supervision and cleanup paths | `test_spec175_real_minindn_gate.py`, G0 production-case census |
| FR-010 | Spec175-to-Spec180 handoff and closure boundary | `handoff-to-spec180.md`, `requirements-v1.md`, and this evidence record |

The inspection confirms that the maintained C++ and Python entrypoints reach the
same streamed invocation and native Provider owners. No generated service/stub
owner, alternate streamed lifecycle, or runtime Transformers/PyTorch path was
introduced by the current source subject. Qwen roles use the canonical
`/LLM/Pipeline/Stage/{index}` identity, and the canonical source binding is
explicitly represented by `canonicalSourceDataName`, `canonicalSourceDigest`,
and `canonicalSourceBytes`.

## Focused red/green evidence

Command:

```text
python3 -m pytest -q \
  tests/python/test_spec175_contract_gate.py \
  tests/python/test_spec175_streamed_generation.py \
  tests/python/test_spec175_conversation.py \
  tests/python/test_spec175_onnx_deployment_boundary.py \
  tests/python/test_spec175_real_minindn_gate.py
```

Result: `149 passed in 5.81s`.

The focused gate covers source-contract mutations, stream ordering and terminal
ownership, Qwen state/continuation and mismatch rejection, native assembly and
deployment boundaries, and the real MiniNDN case registration/cleanup contract.

## Boundary and remaining gates

This PASS means the written Spec175 contracts agree with the current
production source and focused regressions. It does not claim that the complete
native/Python suite or CPU/MiniNDN workload has passed. Those are T021 and T022,
which must run from this unchanged sealed source before T023 can issue
`LOCAL_FUNCTIONAL_PASS` and hand off to Spec180.
