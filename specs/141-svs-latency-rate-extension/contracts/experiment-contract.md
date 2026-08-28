# Experiment Contract

```text
binary_sha256 = a5789d075ec0fbd702add6cc4084bddd0e91cc996b12ca6166e665fd6ec9204a
rates_per_peer = [600, 800]
modes = [face-inline-rsa, worker-rsa]
matrix_order = [600-inline, 600-worker, 800-inline, 800-worker]
timing = 10/60/10
topology = identical to Spec 140
security = identical RSA-2048 path
raw_statistics = [mean, p50, p95, p99]
automatic_retry = false
```

An unsustained cell is retained. Percentiles for partial delivery are explicitly
survivor-distribution metrics and cannot establish successful capacity.
