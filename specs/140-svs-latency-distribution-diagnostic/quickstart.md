# Quickstart

Do not execute the MiniNDN cells until the pre-implementation audit, tests, and
new build identity pass.

```bash
python3 Experiments/build_svs_latency_distribution.py build
python3 tests/python/test_spec140_svs_latency_distribution.py
sudo -n -E taskset -c 0-3 \
  python3 Experiments/NDN_SVS_Latency_Distribution_Minindn.py
python3 Experiments/analyze_svs_latency_distribution.py \
  results/spec140-svs-latency-distribution/<campaign-id>
```

The MiniNDN command creates exactly two 400 pps cells and never writes beneath
`results/spec136-rsa-single-worker/`.
