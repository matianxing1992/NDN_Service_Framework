# Quickstart: Spec 131

This is the planned operator flow. Commands are not acceptance evidence until
the corresponding tasks implement and verify them.

## 1. Validate planning and tool gates

```bash
cd /home/tianxing/NDN/ndn-service-framework
codegraph status .
codegraph status /home/tianxing/NDN/ndn-svs
node /home/tianxing/.codex/gsd-core/bin/gsd-tools.cjs validate health
bash .specify/scripts/bash/check-prerequisites.sh --json --require-tasks
```

## 2. Run contract tests

```bash
python3 -m pytest -q tests/python/test_spec131_svs_pubsub_commit_latency.py
```

The tests must cover exact subjects, matrix cardinality/order, rate schedule,
clock/event joins, censoring, explicit `null` baseline counters, no retry,
dependency exclusion, and claim wording.

## 3. Build pinned subjects on temporary Boost 1.71 branches

```bash
python3 Experiments/build_svs_pubsub_commit_bench.py \
  --ndn-svs /home/tianxing/NDN/ndn-svs \
  --build-root build/spec131 \
  --baseline a9944019f76791773604999f00128057b9534ace \
  --latest 6bb34545b4f89f1f6c265a68c18f1a40ade413eb \
  --boost-version 1.71 \
  --original-boost-minimum 1.74
```

Expected output includes each base and temporary branch/head/tree, the same
canonical two-line `wscript` patch and SHA-256, clean post-commit worktrees,
Boost 1.71 include/library/configure evidence, build commands, library/binary
hashes, `ldd` dependency lists proving no Boost 1.74, self-test results, and
explicit `ndnsfRuntimeDependencies: []`. The temporary branch commits must not
be merged, rebased, or pushed.

## 4. Run non-formal MiniNDN smokes

```bash
sudo -n env PATH="$PATH" python3 \
  Experiments/NDN_SVS_PubSub_Commit_Latency_Minindn.py smoke \
  --subject baseline-sync-serial \
  --output results/spec131-svs-pubsub-commit-latency/smoke-baseline-1000

sudo -n env PATH="$PATH" python3 \
  Experiments/NDN_SVS_PubSub_Commit_Latency_Minindn.py smoke \
  --subject latest-async-parallel \
  --output results/spec131-svs-pubsub-commit-latency/smoke-latest-1000
```

Both subjects must pass the clock probe, dependency check, event accounting,
clean cleanup, and 1000 pps attempted-rate +/-2% gate. Smoke results are never
merged into formal statistics.

## 5. Create and inspect the formal campaign

```bash
python3 Experiments/NDN_SVS_PubSub_Commit_Latency_Minindn.py \
  plan --campaign-id <campaign-id> \
  --output results/spec131-svs-pubsub-commit-latency/<campaign-id>

sudo -n env PATH="$PATH" python3 \
  Experiments/NDN_SVS_PubSub_Commit_Latency_Minindn.py seal \
  results/spec131-svs-pubsub-commit-latency/<campaign-id>
```

Do not start until the validator reports exactly 10 cells, ordinals 1--5 old
and 6--10 latest, unique output paths, and `automaticRetry=false`.

## 6. Execute old first, then latest

```bash
sudo -n python3 Experiments/NDN_SVS_PubSub_Commit_Latency_Minindn.py \
  run-block --campaign results/spec131-svs-pubsub-commit-latency/<campaign-id> \
  --subject baseline-sync-serial

sudo -n python3 Experiments/NDN_SVS_PubSub_Commit_Latency_Minindn.py \
  run-block --campaign results/spec131-svs-pubsub-commit-latency/<campaign-id> \
  --subject latest-async-parallel
```

The second command must refuse to start unless all 5 baseline receipts are
terminal and source/runner/analyzer hashes are unchanged.

## 7. Analyze and close

```bash
python3 Experiments/analyze_svs_pubsub_commit_latency.py \
  results/spec131-svs-pubsub-commit-latency/<campaign-id>
```

Acceptance requires the raw campaign summary, five direct rate comparisons,
delivery/rate guards, highest sustained rate, source
identity, intervening commit list, and explicit version-bundle claim boundary.

## Estimated Runtime

- patch-audited dual build and self-tests: about 10--30 minutes depending on cache;
- two smokes: about 5 minutes;
- formal 10-cell campaign: about 16--22 minutes;
- validation/analysis: about 5--15 minutes.

Formal cells are never selectively rerun. A failed formal campaign remains at
its original path; a correction starts a new complete campaign ID.
