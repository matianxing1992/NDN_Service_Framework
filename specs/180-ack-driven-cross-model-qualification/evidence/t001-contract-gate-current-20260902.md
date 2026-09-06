# Spec180 T001 Contract-Gate Evidence

**Status**: PASS for T001 implementation gate; not a model or deployment
qualification result.

## Reproduction

From the repository root:

```bash
python3 scripts/spec180_contract_gate.py \
  --project-root . \
  --feature-dir specs/180-ack-driven-cross-model-qualification \
  --output specs/180-ack-driven-cross-model-qualification/evidence/t001-contract-gate-current-20260902.json
python3 -m pytest -q tests/python/test_spec180_contract_gate.py
```

The captured gate reported `PASS`, `contractReady=true`, and
`qualificationReady=false` (formal qualification remains blocked), with zero
issues, 24 functional requirements, 9 success criteria, 20 sequential tasks,
complete traceability, and `trustRootStatus=CONFIGURED`. The focused contract suite
reported 7 passed tests. This capture predates FR-025; it is retained as the
original T001 record and is not the current requirement-count authority. The
current gate must be rerun after any requirement change and now reports 25
functional requirements; the 25-count result is recorded by audit iteration 49.

## What is sealed

- The Spec175 handoff and current local closure are present and report
  `LOCAL_FUNCTIONAL_PASS` at source revision
  `286a0098b9bf0dfc2e0b77a320e9e75c38851dc7`.
- The catalogue authority is the checked-in Ed25519 key ID
  `spec180-yolo-catalogue-ed25519-20260902` with public-key digest
  `sha256:a4ca7f79fa9f3ff375fbce189ed7ab15748346b51b555c953abf4c7516f09f31`.
- The external model-manifest authority is separate, with key ID
  `spec180-model-manifest-ed25519-20260902` and public-key digest
  `sha256:6aa8a1c4bcf30fd51445471ea435394bd9a6f8a81a9a7c27ff9e99a63a6945ea`.
- The gate rejects unsigned manifests, unknown key IDs, unsupported
  algorithms, tampered signed content, missing/mismatched public-key files,
  path escapes, and historical evidence marked current.

The public keys are development trust roots for implementation and focused
tests. Private signing material is not committed. T004/T012 must receive the
approved signing interface or secret outside the repository before any signed
catalogue/model artifact is promoted.

## Boundary

This evidence proves only the repository-owned document, identity, and
fail-closed trust-root contract. It performs no model export, NDN request,
staging, SIF build, scheduler submission, or TigerCluster mutation. Formal
qualification remains blocked until T002--T013, the T014 convergence audit, and
the T015 isolated local qualification gate pass.
