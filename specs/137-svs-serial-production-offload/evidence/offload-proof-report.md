# Spec 137: Serial Asynchronous Worker-Offload Result

## Verdict

**Formal classification: `INADMISSIBLE`.**

This campaign does **not** prove that workerization/asynchronous offload is
necessary. It does show a repeatable mechanistic trend: moving the still-serial
Sync production chain to one worker reduced publisher Face production CPU by
about 55% in both admissible pairs. It did not produce the required repeatable
Face-heartbeat or delivery-latency improvement.

The treatment is one combined mechanism:

```text
control:   Face performs serial production inline
treatment: Face asynchronously enqueues production to one serial worker
```

Because queueing and worker execution change together, this experiment cannot
attribute an effect separately to "asynchronous" and "worker" components.
`publishAsync()` was common to both modes.

## Frozen Campaign

- Campaign: `t003-four-core-20260723-03`
- Subject commit: `6bb34545b4f89f1f6c265a68c18f1a40ade413eb`
- Binary SHA-256:
  `c4f3b296137033eb82d0e888bd2cacdf492a0e06609de12c3e1e301547e435ac`
- Workload: one 60 pps publisher, one fixed receiver
- Host: four vCPUs; the sole treatment worker uses one CPU
- Formal order: AB/BA/AB; six cells; 10/60/10 seconds; no retries
- Formal receipts: 6/6
- Delivery ratio: 1.000 in every cell
- `max_active_sync_signers`: 1 in every cell
- Production fallback: 0 in every cell

## Run-Level Results

| Ordinal | Pair | Mode | Attempted pps | Admissible | Face CPU / production (ns) | Heartbeat p99 (ms) | Delivery p99 (ms) |
|---:|---:|---|---:|---|---:|---:|---:|
| 01 | 1 | Face | 57.183 | no | 267,597 | 28.854 | 45.011 |
| 02 | 1 | Worker | 58.283 | no | 114,526 | 18.938 | 41.761 |
| 03 | 2 | Worker | 60.000 | yes | 115,831 | 1.687 | 17.764 |
| 04 | 2 | Face | 60.000 | yes | 261,523 | 1.467 | 16.439 |
| 05 | 3 | Face | 60.000 | yes | 293,673 | 1.794 | 17.697 |
| 06 | 3 | Worker | 60.000 | yes | 132,623 | 1.620 | 17.372 |

Pair 1 failed the pre-registered offered-load gate in both modes:
Face missed target by 4.69% and worker by 2.86%. It is retained and is not
replaced.

## Paired Effects

Positive percentages mean the worker value is lower.

| Pair | Pair admissible | Face CPU relief | Heartbeat p99 change | Delivery p99 change |
|---:|---|---:|---:|---:|
| 1 | no | 57.20% | +34.37% | +7.22% |
| 2 | yes | 55.71% | -15.04% | -8.06% |
| 3 | yes | 54.84% | +9.73% | +1.84% |

The two admissible pairs consistently confirm moved CPU work. They do not meet
the registered responsiveness criterion: neither admissible pair improves
heartbeat p99 by 20%, and one is 15.04% worse. Delivery remains correct; its
p99 changes are small and mixed.

## What Is And Is Not Proven

Measured:

- one worker and one active Sync signer;
- zero fallback and complete accounting;
- about 55% lower Face production CPU in the two admissible pairs;
- 100% delivery in all six cells.

Not proven:

- that worker offload is necessary at 60 pps;
- that it raises maximum PubSub PPS;
- that it improves heartbeat or delivery tail latency;
- that asynchronous queueing and worker execution have separate benefits;
- RSA, publication-signing, multi-worker, or other-host behavior.

The defensible conclusion is: **serial asynchronous worker offload moves
substantial Sync-production CPU off the Face thread, but this campaign does not
establish a necessary or user-visible performance benefit.**

## Evidence Integrity

The sealed offline verifier confirmed one admitted preflight, six unique formal
receipts, and the frozen subject manifest. Receipt 01 and 02 remain
inadmissible; no formal cell was rerun or replaced.
