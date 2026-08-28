# Spec 140 Latency-Distribution Diagnostic Report

## Verdict

`PASS`

The fresh two-cell diagnostic recorded and retained mean, p50, p95, and p99
from raw delivery-latency samples. It does not modify or reconstruct the
frozen Spec 136 result.

## Experiment

```text
topology: two MiniNDN nodes, 100 Mbps, 10 ms one-way, no configured loss
traffic: both nodes publish and subscribe
rate: 400 publications/s per peer
payload: 256 bytes
security: RSA-2048 Data and Sync Interest signing/validation
timing: 10 s warmup, 60 s measurement, 10 s drain
control: Face-inline RSA production
treatment: one FIFO production worker, ordered Face commit
cells: exactly 2, no automatic retry
```

Campaign:

```text
results/spec140-svs-latency-distribution/diagnostic-20260724T000048Z
```

Binary SHA-256:

```text
a5789d075ec0fbd702add6cc4084bddd0e91cc996b12ca6166e665fd6ec9204a
```

## Combined Two-Peer Results

| Mode | Attempted pps/peer | Delivered pps/peer | Delivery | Samples | Mean ms | p50 ms | p95 ms | p99 ms |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| Face-inline RSA | 400.00 | 400.00 | 100.00% | 48,000 | 76.985 | 77.219 | 105.722 | 116.785 |
| One-worker RSA | 400.00 | 400.00 | 100.00% | 48,000 | 58.236 | 56.290 | 80.949 | 97.219 |

Worker reductions relative to Face-inline:

| Statistic | Reduction |
|---|---:|
| Mean | 24.4% |
| p50 | 27.1% |
| p95 | 23.4% |
| p99 | 16.8% |

## Per-Peer Reproducibility

| Mode | Peer | Samples | Mean ms | p50 ms | p95 ms | p99 ms |
|---|---|---:|---:|---:|---:|---:|
| Face-inline | peer-a | 24,000 | 77.010 | 77.324 | 105.635 | 116.460 |
| Face-inline | peer-b | 24,000 | 76.960 | 77.169 | 105.819 | 116.987 |
| One-worker | peer-a | 24,000 | 57.978 | 56.175 | 80.114 | 95.607 |
| One-worker | peer-b | 24,000 | 58.493 | 56.432 | 81.583 | 98.592 |

The analyzer concatenated the two raw peer populations before calculating the
combined percentiles. It did not average peer percentiles.

## Validation

- Both terminal receipts are `COMPLETE`.
- Each peer attempted exactly 24,000 and delivered exactly 24,000 measured
  publications.
- Every summary sample count equals `deliveredMeasured`.
- All summary statistics exactly match offline recomputation from the raw CSV.
- Existing RSA, signer seriality, thread ownership, malformed-object, and
  worker-drain admission checks passed.
- The Spec 140 result tree SHA-256 after analysis is:
  `03950caba86fcdc60c6ca66dc8713b4f1e1fd42625198f8bb6ff0f93ac72154c`.
- The frozen Spec 136 formal tree remains:
  `b074aaa7afc8f0a3fd3c35eb9dcd2fa8e70eb93ee72579d49191f4c230a996f1`.

## Interpretation

At this observed 400 pps boundary, moving serial publication preparation and
RSA signing away from the Face event loop preserved full delivery and reduced
both central delivery latency and tail latency. The worker does not accelerate
RSA itself; it lets Face continue processing incoming Data, Sync state, timers,
and Fetcher callbacks while the serial worker prepares the next publication.

This is one descriptive cell per mode. It demonstrates the requested mechanism
under the recorded conditions, but it is not a repeated-trial confidence
interval or a universal capacity claim.

## Presentation Artifacts

- Email body:
  `docs/PAPER/email-drafts/dr-wang-ndn-svs-20260723/email.md`
- Updated comparison figure:
  `docs/PAPER/email-drafts/dr-wang-ndn-svs-20260723/figure2_400pps_inline_vs_worker.png`
- Machine-readable analysis:
  `results/spec140-svs-latency-distribution/diagnostic-20260724T000048Z/latency-distribution.json`
