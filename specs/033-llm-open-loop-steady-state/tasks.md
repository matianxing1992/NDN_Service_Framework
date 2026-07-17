# Tasks: LLM Open-Loop Steady-State Campaign

## Phase 1: Planning Artifacts

- [x] T001 Create feature plan in `specs/033-llm-open-loop-steady-state/plan.md`.
- [x] T002 Create task list in `specs/033-llm-open-loop-steady-state/tasks.md`.
- [x] T003 Point `.specify/feature.json` and agent context at feature 033.

## Phase 2: Open-Loop User Driver

- [x] T004 Add `--target-rps` and `--open-loop-duration-s` to `user_driver.py`.
- [x] T005 Implement child-process open-loop scheduling with local backpressure accounting.
- [x] T006 Extend workload summary fields with open-loop offered-load metadata.

## Phase 3: Harness And Campaign Plumbing

- [x] T007 Pass open-loop knobs through `Experiments/NDNSF_DI_NativeTracer_Minindn.py`.
- [x] T008 Add open-loop knobs and CSV/summary fields to `run_llm_full_network_campaign.py`.

## Phase 4: Verification

- [x] T009 Compile changed Python scripts.
- [x] T010 Run a small MiniNDN open-loop full-network smoke.
- [x] T011 Record campaign evidence and interpretation.
- [x] T012 Run `git diff --check` and CodeGraph sync/status.

## Evidence

- Context gate: `.specify/memory/constitution.md`,
  `.specify/feature.json`, and this feature plan/tasks were read or updated.
- CodeGraph gate: `codegraph status .` reported an up-to-date index; CodeGraph
  was used to inspect `user_driver.py`,
  `Experiments/NDNSF_DI_NativeTracer_Minindn.py`,
  `run_llm_full_network_campaign.py`, and the Python async collaboration path.
- GSD gate: `node "$GSD_HOME/bin/gsd-tools.cjs" validate health`
  reported healthy.
- Python compile passed:
  `PYTHONPYCACHEPREFIX=/tmp/ndnsf_pycache python3 -m py_compile examples/python/NDNSF-DistributedInference/native_di_tracer/user_driver.py Experiments/NDNSF_DI_NativeTracer_Minindn.py examples/python/NDNSF-DistributedInference/native_di_tracer/run_llm_full_network_campaign.py`.
- CLI sanity passed:
  `PYTHONPATH=pythonWrapper python3 examples/python/NDNSF-DistributedInference/native_di_tracer/user_driver.py --dry-run --requests 4 --concurrency 2 --target-rps 2 --open-loop-duration-s 1`.
- Initial same-process async open-loop MiniNDN attempt failed because the user
  driver log stopped after the first async submit and did not emit
  `NDNSF_DI_NATIVE_TRACER_USER_EXECUTION`. The implementation was adjusted to
  use the existing child-process ServiceUser path with fixed-rate parent
  scheduling and local backpressure accounting.
- MiniNDN open-loop smoke passed:
  `python3 examples/python/NDNSF-DistributedInference/native_di_tracer/run_llm_full_network_campaign.py --out-root /tmp/ndnsf-llm-open-loop-smoke --runs 1 --workloads ol2:4:4 --modes greedy,proportional --stage-execution-delay-scale 4 --target-rps 2 --open-loop-duration-s 2 --provider-check-timeout 60`.
  Results:
  - greedy/ol2: 4/4 success, offeredRps `2.0`, submittedCount `4`,
    localBackpressureCount `0`, p50/p95 `379.634/387.343 ms`.
  - proportional/ol2: 4/4 success, offeredRps `2.0`, submittedCount `4`,
    localBackpressureCount `0`, p50/p95 `577.564/591.556 ms`.
  - Summary path:
    `/tmp/ndnsf-llm-open-loop-smoke/llm-full-network-campaign-summary.json`.
