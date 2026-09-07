# T005 source finding: V3 ONNX backend alternatives

Date: 2026-09-07. Status: focused repair, not runtime qualification.

## Reproduction and cause

The real YOLO adapter declares `(onnxruntime-cpu, onnxruntime-cuda)` per role.
`AutomaticPlanningCoordinator._v3_role_specs` selected only `backends[0]`.
The actual `PreSplitFirstStrategy.propose_v3` accepts exact backends or a
backend family with explicit device suffixes; it correctly does not reinterpret
a CPU-only requirement as CUDA. Thus the projected role lost a declared valid
alternative before the strategy even saw the signed offers.

Nine deterministic tests exercise the verbatim coordinator and strategy
methods against the real V3 contract types and four synthetic Provider views.
Initial tests reproduced three failures: CPU-first rejects GPU BackboneNeck;
CUDA-first rejects all-CPU execution and rejects CPU Merge in mixed execution.
CPU-first/all-CPU control passed. Changing tuple order alone changed failure
location while role coverage and resources stayed unchanged.

Ranked hypotheses were lost backend alternatives, insufficient GPU memory,
and incomplete role cover. The fixture supplies all four roles and sufficient
resources; order sensitivity isolates the first. Additional negative tests
ensure memory, missing-role and wrong-engine constraints still reject.

## Repair boundary and preliminary source review

Keep the `onnxruntime` family unresolved only when the declared backend set is
exactly `{onnxruntime-cpu, onnxruntime-cuda}`. The existing strategy selects the
concrete backend/device from Provider offers. Single-backend and all other sets
remain unchanged. No new protocol field, fake ACK, device override, permission
bypass or fallback is introduced. Recipe identity still binds the declared
requirements; final proposals still carry concrete selected backends/devices.

Owner is the existing DI coordinator, not the Tiger script. The change is in
`NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py`.
It invalidates any prior runtime/source seal for this candidate. Preserve the
four delivered dependency SHAs as delivery provenance, but seal/build the new
experiment runtime source containing this fix. Do not run the old delivery SIF
and claim it includes this correction. T007 full production audit remains open.

## Test evidence and limits

`python3 -m pytest -q tests/python/test_spec183_v3_backend_selection.py Experiments/TigerCluster/tests --tb=short --junitxml=Experiments/TigerCluster/results/spec183-backend-selection-r1/junit.xml`

296 passed in 19.02s (287 Tiger component tests + 9 planning kernel tests).
This predates the explicit normal-import mode added for the later T008 gate;
the final run is recorded separately below.

Final r2, including the explicit normal-import mode: **296 passed in 19.41s**,
exit 0; XML at `Experiments/TigerCluster/results/spec183-backend-selection-r2/junit.xml`.
Normal-import mode itself was not executed successfully; it remains T008.

The receiving host currently lacks `_ndnsf`; a normal planner import fails at
that missing native binding. No native stub was inserted into sys.modules and
no unqualified extension was copied in. Default tests extract the production
methods by AST and use real SDK value types, with synthetic offers/graph inputs.
This is narrower than public APPClient/adapter/signature or GPU integration.
No actual signed ACK, model, Controller/Repo, SIF or Slurm job ran.

After the T008 native/Python build, rerun these same tests with
`SPEC183_REQUIRE_NATIVE_PLANNER_IMPORT=1`; this uses ordinary production imports
and fails (without AST fallback) if the native import is still broken. Real
signed package, four-role CPU/GPU request and canonical assembly remain required
by T009 onward. In particular, portable assembly ABI and final CUDA execution
must be validated against the actual candidate, not inferred from these tests.

## Next

Continue T005 signed material preparation and Controller/Repo readiness, then
T006 collection and the full T004/T002/T007 closure. This source repair does not
close T005 or replace any deployment gate.
