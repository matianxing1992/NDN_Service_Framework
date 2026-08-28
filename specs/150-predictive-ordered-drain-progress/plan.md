# Implementation Plan: Predictive Ordered-Drain Progress

## Scope

Repair only the generic predictive recovery lookup and ordered-delivery state
machine. The public API, payload names, FEC, adaptive controller, and UAV
workload remain frozen.

## Design

Replace advisory `m_draining` use with a serialized drain protocol:

```text
ready/gap event
  -> request drain
  -> one owner loops:
       consume all consecutive terminal gaps
       consume next ready item
       release lock for callback
       advance cursor
       repeat
  -> before release, consume any pending wake
```

An event racing with owner shutdown sets a wake flag under the same mutex.
Late verified cursors behind `nextDeliverCursor` are counted and discarded.

Extend the provider-signed predictive frontier group list with parallel
first/last cursor fields. Validation requires equal vector lengths, ordered
non-overlapping ranges, and canonical names. Recovery selects one containing
range directly; the reverse history walk is removed.

## Validation order

1. deterministic gap-progress and concurrent-wake tests;
2. focused StreamFacade suite and Python field parity;
3. full `./waf build -j2` and all native tests;
4. source/API/workload scans;
5. short impaired smoke;
6. one new immutable two-cell MiniNDN campaign;
7. post-implementation audit.

The exact Spec 149 topology, roles, workload, 5-second warm-up, >=60-second
measurement, loss/reorder profiles, retry settings, and analyzer metrics are
reused without tuning.
