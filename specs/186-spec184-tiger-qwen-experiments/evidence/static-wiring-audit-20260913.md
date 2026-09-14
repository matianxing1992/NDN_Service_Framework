# Spec186 static wiring audit — 2026-09-13

**Scope:** `jobs/spec184/submit.py`, `jobs/spec184/run.sbatch`,
`runtime/spec186_candidate.py`, and all `spec184-*.json` profiles.

The audit found executable-boundary defects before any scheduler or remote
mutation:

| Finding | Repair | Verification |
| --- | --- | --- |
| Host Python/launcher was placed after `--cleanenv --containall`; those paths are not in the SIF namespace. | MiniNDN now stays host-orchestrated and uses the exact-SIF command-provider variables; incomplete Tiger profiles are rejected before an invalid in-image command is emitted. | `test_effective_config_has_explicit_case_transport_and_candidate`, `test_tiger_render_rejects_missing_dispatch_contract` |
| Model/input/identity/permission binds were recorded but not applied to the rendered container argv. | The renderer records declared data inputs for the host command-provider boundary; Tiger dispatch is blocked until a dedicated workload document and per-role argument files can apply those binds safely. | static render guard and pre-dispatch zero-side-effect test |
| Split YOLO profiles were sent as Y-A, and Tiger used unsupported `--run-id/--candidate-digest` arguments. | Map split normal profiles to Y-B and negative profiles to Y-N with the registered `--case` interface. | Effective-config regression |
| Qwen passed the weight file as `--stage-manifest`. | Use the declared `model.stageManifest` asset for local execution; the future Tiger adapter must bind it at `/model/stage-manifest.json`. | Qwen render guard and profile schema checks |
| `local_run` and `run.sbatch` did not consume the rendered environment. | Local children receive the allow-listed environment; Slurm payload validates `SPEC186_EXEC_ENV` and uses `execvpe` with a sanitized environment. | `test_local_passes_rendered_environment_to_child`, shell syntax check |
| Maintained YOLO runner requires a YOLO26n canonical package and eleven `SPEC180_*`/`NDNSF_*` inputs, while profiles declare a bare YOLOv8n file and no runner environment. | Pre-dispatch now reports `HARNESS_MODEL_FAMILY_MISMATCH` and `HARNESS_ENVIRONMENT_UNDECLARED` with zero side effects. | `test_pre_dispatch_exposes_yolo_profile_runner_drift` |

The current collector digest after this source change is
`sha256:7788f23b3edf3d814f2f26edfa18494e561aa1b688136e8204cae494980988f4`;
all eight Spec186 profiles were refreshed to bind it. Existing pre-dispatch
receipts that bind earlier collector digests remain historical and are not
reused as current candidate identities.

The focused Spec186 regression file now passes 18 tests; the complete
TigerCluster test suite remains the authoritative broader check.

This audit is static and component-level. It does not establish a YOLO
MiniNDN, exact-SIF, CUDA, Slurm/Tiger, Qwen, or numerical-oracle PASS. The
canonical package/config/key-map inputs and exact source-sealed base SIF still
must be supplied before T006–T012 can advance.
