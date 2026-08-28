# Quickstart: Synchronous NDN-SVS Stage Profiling

These commands define the implementation acceptance workflow. They are not an
authorization to run the formal matrix before implementation and audit close.

> **Current gate**: the existing driver is superseded. Complete Spec 134's
> single-I/O-thread qualification, then freeze a corrected Spec 133 driver and
> use new manifest/result paths. The old `preflight-01` remains ineligible
> evidence and is not rerun.

## 1. Contract tests

```bash
python3 tests/python/test_spec133_svs_sync_stage_profile.py
```

Expected: exact base identity; no async/parallel API; complete stage registry;
valid span/summary schemas; nested-span accounting; path counters; exact
five-cell manifest; once-only receipts.

## 2. Build the isolated profiled subject

```bash
python3 Experiments/build_svs_sync_stage_profile.py prepare
# T003--T006 implement and validate the profiling patch and driver.
python3 Experiments/build_svs_sync_stage_profile.py finalize
```

`prepare` writes `build/spec133/subject-foundation.json` with the exact base,
Boost-only head/tree, clean library hash, compiler/linkage, Boost 1.71,
compression-disabled proof, and immutable profiling-worktree start.
`finalize` writes `build/spec133/subject-manifest.json` containing the frozen
profiling patch bytes/hash/paths, profiled head/tree and binary/library hashes,
both same-driver binary hashes, driver/logger/stage-registry identity, and
source/binary absence of async or internal parallel behavior.

## 3. Run the non-formal overhead gate

```bash
python3 Experiments/NDN_SVS_Sync_Stage_Profile_Minindn.py overhead-preflight \
  --subject-manifest build/spec133/subject-manifest.json \
  --output results/spec133-svs-sync-stage-profiling/preflight-<id>
```

Expected: exactly one fixed short 1000 pps/peer clean-control arm, one profiled-
binary/profiling-disabled arm, and one profiled-binary/formal-logging arm.
`overhead-receipt.json` must report A-vs-B, B-vs-C, and A-vs-C and satisfy
FR-009. Preflight data is diagnostic and excluded from formal tables.

## 4. Plan and seal five formal cells

```bash
python3 Experiments/NDN_SVS_Sync_Stage_Profile_Minindn.py plan \
  --campaign-id <unique-id> \
  --subject-manifest build/spec133/subject-manifest.json \
  --overhead-receipt results/spec133-svs-sync-stage-profiling/preflight-<id>/overhead-receipt.json \
  --output results/spec133-svs-sync-stage-profiling/<unique-id>

python3 Experiments/NDN_SVS_Sync_Stage_Profile_Minindn.py seal \
  results/spec133-svs-sync-stage-profiling/<unique-id>
```

Inspect before execution: exactly five cells; rates 200/400/600/800/1000;
attempt 1; unique paths; one profiled synchronous subject; identical logger,
stage registry, payload, topology, timers, CPUs, and security profile.

## 5. Execute once

```bash
python3 Experiments/NDN_SVS_Sync_Stage_Profile_Minindn.py run \
  results/spec133-svs-sync-stage-profiling/<unique-id>
```

The runner executes ascending rates and never retries a terminal cell. A crash,
overload, or invalid diagnostic outcome remains visible.

## 6. Analyze

```bash
python3 Experiments/analyze_svs_sync_stage_profile.py \
  results/spec133-svs-sync-stage-profiling/<unique-id>
```

Required outputs:

```text
campaign-summary.json
cell-summary.csv
rate-stage-summary.csv
critical-path-groups.csv
path-frequency.csv
bottleneck-ranking.csv
bottleneck-report.md
limitations.md
```

The Markdown report must explain application-publication work, remaining
Face/io_context work, timer delay, and external-wait rankings separately and
may state `INCONCLUSIVE` when the evidence does not support one primary
bottleneck.
