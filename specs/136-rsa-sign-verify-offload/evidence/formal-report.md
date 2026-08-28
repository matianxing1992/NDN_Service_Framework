# Spec 136 Formal RSA Single-Worker Report

**Date**: 2026-07-23  
**Campaign**:
`results/spec136-rsa-single-worker/formal-r6-20260723T224836Z`  
**Evidence class**: `FORMAL`  
**Overall verdict**: `TRADE_OFF`

## Integrity

- One frozen binary:
  `f6dd0038bddd3eb7f2803a83dda26205b8d5878da3d68cffdde55e22fb70ef27`.
- One NDN-SVS library:
  `7945f22bcdaaef149f4e3cc2a75a39d124e4dfd54d07027cadcc83b4f5b1308f`.
- R6 seal:
  `c1096bcac07ac177b6e4fd6eb89a0a08feb75b65f8291f3e92415821a5d55c32`.
- Exactly ten registered cells ran once in the sealed order.
- Ten unique terminal receipts exist; all are `COMPLETE`, with both peer
  processes returning zero.
- Every cell used two MiniNDN nodes and two simultaneous publish/subscribe
  processes with 10/60/10 timing.
- No formal cell was retried, replaced, tuned, or selectively omitted.
- Campaign tree after the one analyzer invocation:
  `b074aaa7afc8f0a3fd3c35eb9dcd2fa8e70eb93ee72579d49191f4c230a996f1`.

## Formal Cells

| Mode | Rate/peer | Attempted pps/peer | Delivered pps/peer | Ratio | Signer util | Heartbeat p99 | Delivery p99 |
|---|---:|---:|---:|---:|---:|---:|---:|
| inline | 200 | 200.00 | 200.00 | 100% | 32.46% | 2.145 ms | 47.393 ms |
| worker | 200 | 200.00 | 200.00 | 100% | 33.05% | 1.874 ms | 45.201 ms |
| inline | 250 | 250.00 | 250.00 | 100% | 38.03% | 2.618 ms | 57.638 ms |
| worker | 250 | 250.00 | 250.00 | 100% | 38.91% | 2.143 ms | 49.724 ms |
| inline | 300 | 299.98 | 299.98 | 100% | 43.49% | 2.832 ms | 49.795 ms |
| worker | 300 | 299.98 | 299.98 | 100% | 44.66% | 2.779 ms | 50.415 ms |
| inline | 350 | 349.98 | 349.98 | 100% | 49.08% | 3.640 ms | 61.714 ms |
| worker | 350 | 349.98 | 349.98 | 100% | 50.92% | 3.068 ms | 53.952 ms |
| inline | 400 | 400.00 | 400.00 | 100% | 54.50% | 4.166 ms | 128.052 ms |
| worker | 400 | 400.00 | 400.00 | 100% | 57.87% | 7.123 ms | 84.880 ms |

All cells drained worker outstanding, publication Fetch pending, and abandoned
Face calls to zero. All security/accounting gates passed. The 400-pps inline
cell recorded 33 publication Fetch timeouts during measurement, but still
delivered all 48,000 measured publications and drained to zero; this diagnostic
is retained rather than hidden.

## Paired SC-005 Results

| Rate/peer | Heartbeat p99 change | Delivery p99 change | Result |
|---:|---:|---:|---|
| 200 | 12.63% better | 4.62% better | `NOT_USEFUL_AT_RATE` |
| 250 | 18.12% better | 13.73% better | `NOT_USEFUL_AT_RATE` |
| 300 | 1.87% better | 1.25% worse | `NOT_USEFUL_AT_RATE` |
| 350 | 15.73% better | 12.58% better | `NOT_USEFUL_AT_RATE` |
| 400 | 70.96% worse | 33.71% better | `USEFUL_AT_RATE` |

Delivery ratios are identical, so delivery-p99 comparisons are admissible.
At 400 pps/peer the delivery-tail criterion alone crosses the preregistered
20% threshold. The heartbeat regression demonstrates that the worker is not
uniformly beneficial.

## Interpretation

The worker did not extend delivered capacity within the formal 200–400
pps/peer range because both modes sustained every registered rate. It produced
one bounded delivery-tail benefit at 400 pps/peer, but that benefit was not
adjacent to another `USEFUL_AT_RATE` outcome and coincided with worse Face
heartbeat p99 and release lateness.

RSA itself was not accelerated. At 400 pps/peer the mean Data-sign crypto
service time was approximately 540.94 us inline and 577.84 us with the worker;
Sync Interest signing and RSA validation remained Face-path work. The worker
changes where publication construction, encoding, and the two Data signatures
execute, while adding queue and handoff delay.

SC-006 is therefore not met. The formal conclusion is `TRADE_OFF`, not
`ASYNC_SINGLE_WORKER_USEFUL`, not a production-readiness claim, and not a
rejection of off-Face publication preparation in all workloads.

## Evidence Files

- `campaign-manifest.json`:
  `2c59020a99fc789fa84bb5f67e828afe81a40b8a3ebfdb9514f3bdcede585ecc`
- `campaign-terminals.json`:
  `c40c11a7931d40bdea06cf876c5f55ff4bba7e830cff4e12a20a065fa48e415a`
- `analysis.json`:
  `43632f8b49c1e1f63f9fbc2bbb83b46f9679295cf5d0bc928e9ba71f729b4b28`
- `comparison.md`:
  `d0f8961348305b91206b07e5c5c77f3437293eb4733a1e3b59dcecefb2921d4a`
