# Spec 139 Fixed-Rate Single-Worker Proof

## Verdict

**`TRADE_OFF`**

This result evaluates asynchronous single-worker NDN-SVS Sync-production offload. `publishAsync()` is common to both modes; RSA, multi-worker, cross-version, and universal-necessity claims are excluded.

## Run-Level Results

| Ordinal | Pair | Mode | Attempted pps | Delivery | Face CPU/prod ns | Heartbeat p99 ms | Delivery p99 ms | Admissible |
|---:|---:|---|---:|---:|---:|---:|---:|---|
| 1 | 1 | face-serial | 594.233 | 1.000000 | 219141 | 1.881 | 21.158 | yes |
| 2 | 1 | worker-serial | 592.600 | 1.000000 | 92139 | 2.335 | 33.723 | yes |
| 3 | 2 | worker-serial | 591.917 | 1.000000 | 91076 | 2.331 | 27.607 | yes |
| 4 | 2 | face-serial | 593.017 | 1.000000 | 217612 | 2.123 | 25.697 | yes |
| 5 | 3 | face-serial | 593.450 | 1.000000 | 221463 | 2.123 | 24.322 | yes |
| 6 | 3 | worker-serial | 591.317 | 1.000000 | 91921 | 2.247 | 27.993 | yes |

## Paired Effects

| Pair | Face CPU relief | Heartbeat p99 improvement | Delivery-ratio change | Delivery-p99 improvement |
|---:|---:|---:|---:|---:|
| 1 | 57.95% | -24.17% | +0.0000 | -59.39% |
| 2 | 58.15% | -9.78% | +0.0000 | -7.43% |
| 3 | 58.49% | -5.88% | +0.0000 | -15.09% |

## Registered Decision Predicates

- All six admissible: `True`
- Face CPU relief >=50% pairs: `3`
- Heartbeat improvement >=20% pairs: `0`
- Delivery-ratio no-harm: `True`
- Delivery-p99 harm >10% pairs: `2`
- One-worker queue clean: `True`

The raw run, paired, stage, traffic, host-load, and conclusion artifacts are under the campaign `analysis/` directory.
