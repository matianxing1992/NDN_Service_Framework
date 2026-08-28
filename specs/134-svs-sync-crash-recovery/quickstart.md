# Quickstart: Spec 134

## 1. Read-only source audit

```bash
git -C /home/tianxing/NDN/ndn-svs show \
  a9944019f76791773604999f00128057b9534ace:README.md
git -C /home/tianxing/NDN/ndn-svs grep -n -E \
  'thread|processEvents|publish\\(' \
  a9944019f76791773604999f00128057b9534ace -- \
  README.md examples ndn-svs tests
```

Expected: README/header silence, cross-thread example claim, no concurrent unit
test.

## 2. Contract tests

```bash
python3 tests/python/test_spec134_svs_sync_crash_recovery.py
```

Before T003 completion, tests requiring the corrected single-I/O driver are
expected to fail or be absent; do not run qualification.

## 3. Build clean qualification subject

After T003 passes:

```bash
python3 Experiments/build_svs_sync_crash_recovery.py \
  build-io-qualification
```

The manifest must contain the exact historical base plus only the canonical
Boost 1.71 patch. Any repair commit or profiling patch blocks execution.

## 4. Qualify exactly once

```bash
python3 Experiments/NDN_SVS_Sync_Crash_Recovery_Minindn.py \
  qualify-io \
  --subject-manifest build/spec134/io-qualification-manifest.json \
  --output results/spec134-svs-sync-crash-recovery/io-qualification-<id>
```

Do not reuse a path, retry a negative result, or promote this output into Spec
133's five formal cells.
