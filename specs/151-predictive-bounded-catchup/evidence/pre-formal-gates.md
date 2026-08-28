# Pre-formal Gates

Verdict: **PASS**

- Full Waf build: 367 targets, PASS, 11m 9.677s.
- Native tests: 395/395, PASS.
- Predictive consumer stress loop after Face-thread bridge correction: 20/20,
  PASS.
- Python facade tests: 7/7, PASS.
- UAV Drone, UAV Ground Station, and Python binding resolve the repository
  `build/` Core and Boost 1.71.
- Core workload-special-case branch scan: zero matches.
- Temporary diagnose debug-tag scan: zero matches.
- Runner preflight: PASS, 35 source hashes and 5 binary hashes.

Frozen formal root:

```text
results/spec151-predictive-bounded-catchup-formal-20260726T071901Z
```

Frozen campaign SHA-256:

```text
9bc16c1ca0ae06a34913ead22f442ab037125b06e8b5216fcf94cf285381ff87
```

The campaign permits no automatic retry and no rerun.

The earlier `20260726T071826Z` directory was prepare-only and was never
executed because its source inputs changed before launch. It is explicitly
marked as non-evidence.
