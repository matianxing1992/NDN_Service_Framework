# Tasks: LLM Open-Loop RPS Sweep

## Phase 1: Planning

- [x] T001 Create feature plan in `specs/034-llm-open-loop-rps-sweep/plan.md`.
- [x] T002 Create task list in `specs/034-llm-open-loop-rps-sweep/tasks.md`.
- [x] T003 Point `.specify/feature.json` and agent context at feature 034.

## Phase 2: Campaign Runner

- [x] T004 Add `--target-rps-series` parsing to `run_llm_full_network_campaign.py`.
- [x] T005 Auto-size open-loop request caps from rate and duration.
- [x] T006 Add rate-labeled workload names to per-run rows and summaries.

## Phase 3: Verification

- [x] T007 Compile changed Python scripts.
- [x] T008 Run a short MiniNDN two-rate smoke.
- [x] T009 Run a 1/2/4/8 RPS greedy/proportional sweep if the smoke passes.
- [x] T010 Record evidence and interpretation.
- [x] T011 Run `git diff --check` and CodeGraph sync/status.

## Evidence

- Context gate: `.specify/feature.json`,
  `.specify/memory/constitution.md`, and feature 033 artifacts were read before
  starting this continuation.
- CodeGraph gate: `codegraph status .` reported a fresh index, and CodeGraph
  was used to inspect `run_llm_full_network_campaign.py` before edits.
- GSD gate: `node "$GSD_HOME/bin/gsd-tools.cjs" validate health`
  reported healthy.
- Python compile passed:
  `PYTHONPYCACHEPREFIX=/tmp/ndnsf_pycache python3 -m py_compile examples/python/NDNSF-DistributedInference/native_di_tracer/run_llm_full_network_campaign.py Experiments/NDNSF_DI_NativeTracer_Minindn.py examples/python/NDNSF-DistributedInference/native_di_tracer/user_driver.py`.
- CLI help sanity passed and showed `--target-rps-series`.
- Two-rate MiniNDN smoke passed:
  `python3 examples/python/NDNSF-DistributedInference/native_di_tracer/run_llm_full_network_campaign.py --out-root /tmp/ndnsf-llm-rps-series-smoke --runs 1 --workloads base:4:4 --modes greedy,proportional --stage-execution-delay-scale 4 --target-rps-series 1,2 --open-loop-duration-s 2 --provider-check-timeout 60`.
- Full 1/2/4/8 RPS MiniNDN sweep completed without aborting on failed rates:
  `python3 examples/python/NDNSF-DistributedInference/native_di_tracer/run_llm_full_network_campaign.py --out-root /tmp/ndnsf-llm-rps-sweep-1-2-4-8 --runs 1 --workloads base:8:8 --modes greedy,proportional --stage-execution-delay-scale 4 --target-rps-series 1,2,4,8 --open-loop-duration-s 5 --provider-check-timeout 60`.
  Summary path:
  `/tmp/ndnsf-llm-rps-sweep-1-2-4-8/llm-full-network-campaign-summary.json`.

## 1/2/4/8 RPS Result Snapshot

| key | success rate | success/scheduled | submitted | local backpressure | observed success rps | observed p50/p95 ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| greedy/base-r1 | 1.00 | 5/5 | 5 | 0 | 0.371 | 371.049 / 462.143 |
| greedy/base-r2 | 0.80 | 8/10 | 8 | 2 | 0.616 | 346.923 / 433.181 |
| greedy/base-r4 | 0.40 | 8/20 | 8 | 12 | 0.713 | 0.000 / 398.307 |
| greedy/base-r8 | 0.20 | 8/40 | 8 | 32 | 0.722 | 0.000 / 1086.761 |
| proportional/base-r1 | 1.00 | 5/5 | 5 | 0 | 0.366 | 565.791 / 630.849 |
| proportional/base-r2 | 0.80 | 8/10 | 8 | 2 | 0.607 | 558.379 / 609.244 |
| proportional/base-r4 | 0.40 | 8/20 | 8 | 12 | 0.700 | 0.000 / 598.434 |
| proportional/base-r8 | 0.20 | 8/40 | 8 | 32 | 0.731 | 0.000 / 902.585 |

## Interpretation

- The `--target-rps-series` implementation works: result directories and
  summary keys are rate-labeled (`base-r1`, `base-r2`, `base-r4`, `base-r8`),
  and open-loop request caps are auto-sized from rate and duration.
- With the current robust child-process ServiceUser driver and `concurrency=8`,
  the campaign becomes driver-limited above 1 RPS. Both greedy and proportional
  submit at most 8 requests; additional scheduled requests are local
  `local-open-loop-backpressure` drops.
- This sweep should not be used as a provider-throughput conclusion. It shows
  the next engineering bottleneck: build a lighter open-loop driver that can
  keep many in-flight requests without one child process per request, or run a
  high-concurrency driver sweep to separate driver capacity from provider
  capacity.

- Final checks passed: `git diff --check`; `codegraph sync . && codegraph status .` reported the index is up to date.
