# Experiment Contract

```text
campaign_kind = latency-distribution-diagnostic
rate_per_peer = 400 publications/s
modes = [face-inline-rsa, worker-rsa]
matrix_cells = 2
automatic_retry = false
topology = two MiniNDN nodes, 10 ms one-way, 100 Mbps, 0% configured loss
direction = both peers publish and subscribe
timing = 10/60/10 seconds
cpu_affinity = 0-3
data_and_interest_security = RSA-2048
publication_workers = [0, 1]
receive_workers = 0
publication_fetch_window = 64
sync_batch_window = 5 ms
```

Every peer must retain:

```text
delivery-latency.csv:
  latencyNs

peer-summary.json:
  deliverySamples
  deliveryMeanNs
  deliveryP50Ns
  deliveryP95Ns
  deliveryP99Ns
```

Admission:

```text
392 <= attemptedMeasured / 60 <= 408
deliverySamples == deliveredMeasured
summary statistics == recomputed raw-sample statistics
RSA sign/validate observed
maxActiveSigners == 1
invalid == 0
selfDeliveries == 0
worker/Face ownership checks pass
```

No result may be copied from, written into, or retrofitted onto a Spec 136
campaign.
