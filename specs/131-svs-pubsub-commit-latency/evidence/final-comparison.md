# Spec 131 Final Comparison

The corrected campaign compares the two pinned version bundles with one direct
observation per rate. It does not isolate async publication or threading and
does not provide replication-based statistical confidence.

| Rate | Baseline scheduled/attempted/API/delivered | Latest scheduled/attempted/API/delivered | Baseline delivered/attempted | Latest delivered/attempted | Baseline attempted pps | Latest attempted pps | Baseline p50/p95/p99 ms | Latest p50/p95/p99 ms | Direct p95 delta ms | Result |
|---:|:---|:---|---:|---:|---:|---:|---:|---:|---:|:--|
| 200 | 12000/11998/11998/11998 | 12000/12000/12000/12000 | 100.000% | 100.000% | 199.967 | 200.000 | 12.105/13.602/17.137 | 11.061/12.379/13.346 | -1.223 | descriptive improved |
| 400 | 24000/24000/24000/24000 | 24000/23847/23847/23847 | 100.000% | 100.000% | 400.000 | 397.450 | 12.187/14.566/16.653 | 11.101/14.006/19.894 | -0.560 | inconclusive |
| 600 | 36000/35998/35998/35998 | 36000/36000/36000/36000 | 100.000% | 100.000% | 599.967 | 600.000 | 12.508/15.670/17.774 | 10.886/12.804/15.776 | -2.866 | descriptive improved |
| 800 | 48000/47958/47958/47892 | 48000/47578/47578/47517 | 99.862% | 99.872% | 799.300 | 792.967 | 13.665/20.779/28.139 | 11.026/21.038/372.780 | +0.259 | inconclusive |
| 1000 | 60000/59997/59997/3587 | 60000/37593/37593/37593 | 5.979% | 100.000% | 999.950 | 626.550 | 38618.956/66827.813/69457.729 | 14.587/30.125/61.703 | -66797.689 | sender-limited; inconclusive |

Highest sustained rate under the preregistered criteria is 600 pps for both
subjects. At 800 pps both attempted rates are valid but each has nonzero
missing items. At 1000 pps the baseline offered load is valid but delivery
collapses to 5.979%; the latest cell is sender-limited at 626.55 attempted pps,
so no 1000 pps latest-capacity claim is allowed.

Intervening treatment-bundle commits:

1. `a8a9656` parallel receive processing and local Sync batching API
2. `15d1bc6` parallel production and ordered async PubSub publishing
3. `64a4476` V2 signed-Interest implementation change
4. `6222a4a` V3 synchronization protocol
5. `64b6ff7` sparse mapping recovery
6. `bc75baf` failure-atomic segmented publication
7. `6bb3454` bounded segmented fetch and repair recovery

Canonical generated artifacts:

- `rate-comparison.md` SHA-256: `a7c14f075de4d66f03587a90fb6c911020ec4af1c4b31496c1648e4274363a3a`
- `campaign-summary.json` SHA-256: `a14f65a0b7b967b31961533232fa2de663b7c11980da4affbf5b58ff6a3953a0`
- `cell-comparison.csv` SHA-256: `66362d3c755d445eed8a9bc29929b8b6c99382eabe1a45ee340bed2ddc42ecc8`
