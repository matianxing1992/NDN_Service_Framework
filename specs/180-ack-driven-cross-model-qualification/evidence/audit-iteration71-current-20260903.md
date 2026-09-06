# Spec180 Audit Evidence — Iteration 71

**Date**: 2026-09-03  
**Subject**: current `Experimental` worktree after semantic YOLO cut repair  
**Mode**: implementation checkpoint; not T014 convergence and not qualification

## Verdict

**CONDITIONAL PASS FOR IMPLEMENTATION; FORMAL VALIDATION BLOCKED**.

The YOLO exporter and adapter no longer use node-count percentages or fixed
topological indexes for shared-role cuts. A canonical package records semantic
architecture scopes, lifted-constant ownership, producer/consumer tensor
interfaces, role dependency edges, safe cuts, and the full-model equivalence
oracle. The adapter revalidates those records against the loaded ONNX graph
before creating a runtime candidate. Native assembly, the live NFD/NDN-SVS
driver, production result writing, MiniNDN, SIF, and Tiger remain unexecuted.

## Reproducible checks

| Check | Result |
|---|---|
| `python3 -m py_compile tools/ndnsf-di/export_spec180_yolo26_onnx.py NDNSF-DistributedInference/ndnsf_distributed_inference/adapters/yolo/{candidates,adapter}.py` | **PASS** |
| `PYTHONPATH=NDNSF-DistributedInference:pythonWrapper pytest -q tests/python/test_spec180_yolo_ack_planning.py tests/python/test_spec180_yolo_export.py tests/python/test_spec180_yolo_adapter.py` | **19 passed** |
| `PYTHONPATH=NDNSF-DistributedInference:pythonWrapper pytest -q tests/python/test_spec180_yolo_export.py::test_real_export_produces_external_initializer_manifest` | **1 passed** |
| `PYTHONPATH=NDNSF-DistributedInference:pythonWrapper pytest -q tests/python/test_spec180_yolo_adapter.py::test_semantic_partition_is_runtime_bound_not_a_catalogue_hint` | **1 passed** |
| `python3 .agents/skills/speckit-audit/scripts/audit_speckit_structure.py specs/180-ack-driven-cross-model-qualification --strict` | **PASS** |
| `python3 scripts/spec180_contract_gate.py --project-root . --feature-dir specs/180-ack-driven-cross-model-qualification` | **PASS; contractReady=true, qualificationReady=false** |

The export/adapter checks include an altered-partition regression and confirm
that the generated catalogue contains semantic cuts rather than `afterNode`
or percentage-derived boundaries. The oracle metadata explicitly distinguishes
the PyTorch reference from the CPU ONNX Runtime comparison. These checks do
not execute NDN or claim a numeric YOLO equivalence result for a deployed
shared graph.

## Remaining blockers

1. Wire the semantic partition and external initializer contract into native
   `RoleAssemblySpec`/assembler and the maintained application (T007--T009).
2. Implement and test the candidate-bound in-image dispatcher and actual
   NFD/NDN-SVS ACK-to-Response driver (T011).
3. Wire structured terminal-result records into the production result writer
   (T013/T020).
4. Re-run T014 design-code convergence; only a fresh `PASS` unlocks local
   inventory, SIF, and Tiger gates.
