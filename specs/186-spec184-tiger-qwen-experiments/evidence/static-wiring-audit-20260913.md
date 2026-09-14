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
| Nested profile fields and terminal receipts were under-validated: unknown nested keys, off-topology roles, duplicate identities, marker-only PASS and stale run roots could pass the boundary. | Strict nested schema/resource/role checks, candidate-bound terminal evidence, stable malformed-input errors and run-root reservation now fail closed before execution. | `test_profile_mutations_fail_closed`, `test_terminal_collector_rejects_marker_only_pass`, `test_run_sbatch_rejects_run_root_reuse` |
| Role GPU/backend/service semantics, per-role fallback policy, duplicate endpoint hosts, allocation placeholders, Qwen format/entrypoint drift and diagnostic command overrides were not bound to execution. | Profile validation binds role semantics, ordered stage dependencies and role/runtime fallback equality; pre-dispatch rejects placeholder resources and Qwen GGUF/ONNX harness drift; Slurm consumes memory/GPU requests; command overrides cannot produce qualification PASS. | focused profile/harness/submit/local regressions |
| Slurm created the run-owned directories but left the child working directory and `HOME` at shared job context paths. | The payload now changes directory into the reserved run root and scopes `HOME` to its private `home/` child before `execvpe`, preventing cross-run PIB/TPM and evidence leakage. | `test_run_sbatch_scopes_home_to_run_root`, shell syntax check |
| Scheduler child identity variables were syntax-checked but not value-bound to the effective config. | `run.sbatch` rejects mismatched `SPEC186_RUN_ID` and `SPEC186_CANDIDATE_DIGEST` before creating the run root. | `test_run_sbatch_rejects_identity_env_drift_before_run_root_creation` |

The current collector digest after this source change is
`sha256:847b5261065419bf6467136f9d9739898409bf2cdd05d9a6214644bc40582490`;
all eight Spec186 profiles were refreshed to bind it. Existing pre-dispatch
receipts that bind earlier collector digests remain historical and are not
reused as current candidate identities.

The focused Spec186 regression file now passes 44 tests; the complete
TigerCluster test suite passes 106 tests. The terminal collector additionally
requires a GPU-backed YOLO receipt to contain exactly three CUDA model roles
and a CPU Merge role when GPU evidence is present; malformed all-CPU or
misassigned role receipts remain rejected. These are static/component checks,
not runtime qualification.

This audit is static and component-level. It does not establish a YOLO
MiniNDN, exact-SIF, CUDA, Slurm/Tiger, Qwen, or numerical-oracle PASS. The
canonical package/config/key-map inputs and exact source-sealed base SIF still
must be supplied before T006–T012 can advance.
