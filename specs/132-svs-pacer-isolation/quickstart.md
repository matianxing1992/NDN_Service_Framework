# Quickstart: Bidirectional Capability Comparison

## Contract tests

```bash
python3 tests/python/test_spec132_svs_bidirectional_commit_latency.py
```

Expected: direct main-thread `publish()`/`publishAsync()`, two publishing peers,
no harness queue/post, direction-aware analysis, and exact 10-cell manifest.

## Build immutable subjects

```bash
python3 Experiments/build_svs_pubsub_commit_bench.py
```

Expected authority: `build/spec132/subjects.json`, exact base commits, identical
Boost 1.71 patch hash, clean temporary worktrees, and two passing self-tests.

## Plan and seal one new campaign

```bash
python3 Experiments/NDN_SVS_PubSub_Commit_Latency_Minindn.py plan \
  --campaign-id <unique-id> \
  --output results/spec132-svs-pacer-isolation/<unique-id>
python3 Experiments/NDN_SVS_PubSub_Commit_Latency_Minindn.py seal \
  results/spec132-svs-pacer-isolation/<unique-id>
```

Inspect the sealed manifest before execution: exactly 10 cells, ordinals 1-5
synchronous and 6-10 asynchronous, rates 200 through 1000, attempt 1.

## Execute once

```bash
python3 Experiments/NDN_SVS_PubSub_Commit_Latency_Minindn.py run-block \
  results/spec132-svs-pacer-isolation/<unique-id> \
  --subject sync-publish-no-internal-parallelism
python3 Experiments/NDN_SVS_PubSub_Commit_Latency_Minindn.py run-block \
  results/spec132-svs-pacer-isolation/<unique-id> \
  --subject async-publish-parallel-sync
```

The treatment command is blocked until all five baseline receipts exist. A
failed subject cell remains terminal and is not retried.

## Analyze

```bash
python3 Experiments/analyze_svs_pubsub_commit_latency.py \
  results/spec132-svs-pacer-isolation/<unique-id>
```

Expected outputs include cell summaries, a direction table, five direct rate
comparisons, and the synchronous tested-grid sustainable ceiling.
