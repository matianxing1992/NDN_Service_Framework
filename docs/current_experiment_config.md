# Current NDNSF/SVS Performance Baseline

Date: 2026-05-27

This file records the current low-latency MiniNDN baseline used before adding
the Python wrapper layer.

## Software Baseline

- NDNSF repository: `ndn-service-framework`
- NDN-SVS repository: `/home/tianxing/NDN/ndn-svs`
- NDN-SVS runtime library installed through `sudo ./waf install && sudo ldconfig`
- NDN-SVS publish path uses async publication workers:
  - async publish API enabled by default in NDNSF
  - publication worker pool prepares Data signing, wire encoding, DataStore insert,
    and mapping insert off the Face/io_context thread
  - Face/io_context thread keeps only final `face.put()` and ordered `updateSeqNo()`
- NDNSF SVS defaults:
  - `NDNSF_SVS_ASYNC_PUBLISH=1`
  - `NDNSF_SVS_PARALLEL_SYNC=1`
  - `NDNSF_SVS_PARALLEL_SYNC_WORKERS=4`
  - `NDNSF_SVS_PARALLEL_SYNC_QUEUE=256`
  - `NDNSF_SVS_PARALLEL_PRODUCTION=1`
  - `NDNSF_SVS_PARALLEL_PRODUCTION_WORKERS=4`
  - `NDNSF_SVS_PARALLEL_PRODUCTION_QUEUE=256`
  - `NDNSF_SVS_MAX_SUPPRESSION_MS=1`
- NDNSF message crypto:
  - hybrid message encryption enabled by default
  - direct NAC-ABE message encryption mode removed from the default runtime path

## MiniNDN Experiment Configuration

- Topology: `Experiments/Topology/testbed(loss=0%).conf`
- User node: `memphis`
- Controller node: `memphis`
- Provider node: `ucla`
- Providers: 1
- Strategy: `first-responding`
- Workload: open-loop
- Adaptive admission: disabled for raw throughput/latency baseline
- Request timeout: 5000 ms
- Drain time: 10 s for the 30 s run, 5 s for quick checks
- Provider certificate server: enabled

Representative command:

```bash
sudo -n python3 Experiments/NDNSF_NewAPI_Minindn_Perf.py \
  --topology-file 'Experiments/Topology/testbed(loss=0%).conf' \
  --user-node memphis \
  --controller-node memphis \
  --provider-nodes ucla \
  --providers 1 \
  --rate-series 70,100,150 \
  --workload-mode open-loop \
  --strategy first-responding \
  --per-rate-duration 30 \
  --max-total-runtime-seconds 600 \
  --request-timeout-ms 5000 \
  --timeout-ms 5000 \
  --drain-seconds 10 \
  --disable-adaptive-admission-control \
  --serve-provider-certs
```

## Baseline Results

Installed-library verification run:
`results/ndnsf_publish_worker_batch_update_no_admission_verify_70_150_20260527/`

| Offered RPS | Actual RPS | Success | Avg Latency | P50 | P95 | P99 | Timeout |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 70 | 69.87 | 100% | 180.89 ms | 170.71 ms | 249.19 ms | 377.23 ms | 0 |
| 100 | 99.73 | 100% | 183.99 ms | 170.38 ms | 259.00 ms | 407.59 ms | 0 |
| 150 | 148.53 | 100% | 221.06 ms | 189.47 ms | 375.12 ms | 523.07 ms | 0 |

After fixing the App_User actual-rate calculation, a no-admission 70 RPS quick
check reported:

| Offered RPS | Actual RPS | Success | Avg Latency | P50 | P95 | P99 | Timeout |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 70 | 70.00 | 100% | 187.95 ms | 169.02 ms | 331.90 ms | 548.68 ms | 0 |

The quick check is only for validating the Actual RPS statistic because its
10-second duration is too short for stable latency comparison.
