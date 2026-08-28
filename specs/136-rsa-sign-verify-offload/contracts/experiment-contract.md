# Formal Experiment Contract

## Single Subject And Intervention

- Source base:
  `6bb34545b4f89f1f6c265a68c18f1a40ade413eb`.
- Build exactly one binary before formal execution.
- Control runtime mode: `face-inline-rsa`, publication worker count `0`.
- Treatment runtime mode: `worker-rsa`, publication worker count `1`.
- Each peer uses one independent APP pacer. Control posts publication calls to
  Face; treatment calls byte `publishAsync()` directly from the APP pacer.
- Both modes enable the same existing 5 ms local-publication Sync batching.
- The binary hash, source tree, topology, workload, RSA setup, validation,
  instrumentation, and NDN-SVS options are otherwise identical.
- No rebuild, worktree switch, source edit, or configuration tuning is allowed
  after the formal manifest seals.

## Two-Node Bidirectional Unit

Every cell has exactly two MiniNDN nodes and two independent processes.

```text
A publishes R publications/s and subscribes to B
B publishes R publications/s and subscribes to A
aggregate offered publication rate = 2R publications/s
```

Both peers MUST use the cell's same runtime mode. A single-node, one-way, or
mixed-mode run is invalid. Rates are always per peer.

Each peer MUST subscribe to the other peer's publication namespace, not the
shared parent namespace. With publication names
`/spec136/publication/<peer>/<logical-id>`, A subscribes to
`/spec136/publication/peer-b` and B subscribes to
`/spec136/publication/peer-a`. Fetching and discarding a peer's own
publications after delivery is an invalid workload implementation.

## Matrix

| Cell | Mode | Rate/peer | Aggregate |
|---:|---|---:|---:|
| 01 | face-inline-rsa | 200 | 400 |
| 02 | worker-rsa | 200 | 400 |
| 03 | worker-rsa | 250 | 500 |
| 04 | face-inline-rsa | 250 | 500 |
| 05 | face-inline-rsa | 300 | 600 |
| 06 | worker-rsa | 300 | 600 |
| 07 | worker-rsa | 350 | 700 |
| 08 | face-inline-rsa | 350 | 700 |
| 09 | face-inline-rsa | 400 | 800 |
| 10 | worker-rsa | 400 | 800 |

Every cell has one attempt and one terminal receipt. Retry, replacement,
adaptive capacity search, or selective omission is forbidden.

## Fixed Controls

- two processes with independent MiniNDN `HOME`, PIB, TPM, and working paths;
- persistent RSA-2048 identities;
- RSA-signed publication Data and RSA-signed Sync Interests;
- real peer-certificate validation before data delivery/state processing;
- `maxPiggyDataSize=1`, 256-byte deterministic application payload;
- publication Fetch window `64` in both modes;
- 10 ms delay, 100 Mbps, zero configured loss;
- one Face/io_context thread per process;
- parallel Sync processing/production and unrelated workers disabled;
- same four-core host cpuset/scheduling conditions;
- 10 s warmup, 60 s measurement, 10 s drain.

## Blocking Admission Gates

1. Same binary hash and exact runtime delta `workers=0` versus `workers=1`.
2. Exactly zero publication workers in control and one FIFO worker per peer in
   treatment.
3. `SignatureSha256WithRsa` proved on publication Data and Sync Interests.
4. Valid Data/Interest accepted; tampered Data/Interest rejected.
5. No invalid delivery, ordering gap, silent fallback, or accounting loss.
6. Bidirectional MiniNDN routes and delivery proved.
7. No-op pacer reaches 980-1020 attempted pps on each peer at target 1000.
8. Face and APP-pacer thread identities differ on both peers; control calls the
   API on Face and treatment calls it on the APP pacer.
9. Every measured cell independently reaches 98%-102% of target attempted pps
   on each peer. Aggregate averaging cannot satisfy this gate.
10. Each peer reports zero self-publication deliveries.
11. Separate signer mutex-wait and crypto-service counters establish estimated
    serial-signer utilization no greater than 90% at each formal rate.
12. Every peer ends drain with zero worker outstanding, zero uncommitted
    accepted publications, and zero abandoned Face calls.
13. Repeated Mapping announcements MUST NOT duplicate a pending subscription.
    `deliveredMeasured/attemptedMeasured < 0.98` is `LOAD_UNSUSTAINED`; it is a
    measured capacity outcome, not successful completion or harness failure.
14. Delivery p99 is an independent paired metric only when the two delivery
    ratios differ by no more than one percentage point. Otherwise it MUST be
    reported as `delivered-only` with the survivor-set warning.
15. The analyzer MUST preserve both the recorded terminal status and any later
    evidence-based interpretation. It MUST NOT silently replace an immutable
    terminal receipt.

## Terminal Evidence

Allowed statuses are `COMPLETE`, `SECURITY_INVALID`, `ORDER_INVALID`,
`PROCESS_FAILED`, `TIMEOUT`, `ROUTE_INVALID`, `HARNESS_INVALID`, and
`RESOURCE_INVALID`, plus `LOAD_UNSUSTAINED`. All statuses remain evidence.

## Claim Rule

Use SC-005 and SC-006 exactly. Report one of `ASYNC_SINGLE_WORKER_USEFUL`,
`NO_CLEAR_BENEFIT`, `TRADE_OFF`, `REGRESSION`, or `INCONCLUSIVE`. Do not
describe Face-thread relief as RSA acceleration or production readiness.
The retained 800/1000 RSA cells are `OVERLOAD_INVALID`; the retained 600-pps
four-core cell is `RESOURCE_INVALID`. None is a formal cell.

A non-formal two-cell confirmation uses a separate
`DESCRIPTIVE_400_CAPACITY_EXTENSION` or
`NO_DESCRIPTIVE_400_PPS_BENEFIT` verdict. It cannot satisfy SC-006. Wording
must say that the worker moves publication construction, encoding, and Data RSA
signing off Face; Sync Interest signing, RSA validation, and commit remain
Face-path work.
