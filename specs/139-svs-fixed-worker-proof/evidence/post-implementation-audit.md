# Post-Implementation Audit

**Verdict**: PASS for experiment closure; the registered necessity claim is
**not supported**.

## Scope and Integrity

- Campaign:
  `results/spec139-svs-fixed-worker-proof/confirmation01-20260723`
- NDN-SVS commit:
  `6bb34545b4f89f1f6c265a68c18f1a40ade413eb`
- Common binary SHA-256:
  `c4f3b296137033eb82d0e888bd2cacdf492a0e06609de12c3e1e301547e435ac`
- Runner SHA-256:
  `01c99ab1182c441e1b67a129f0da1ce256c3572cad714c8f463cdd32d89a7230`
- Fixed rate: 600 pps
- Formal order: Face/worker, worker/Face, Face/worker
- Formal windows: six fresh 10/60/10 cells
- Formal retry counts: all zero
- Formal receipt count: exactly six
- All six cells: complete and admissible
- Protected Spec 137/138 evidence digest:
  `c2a9339e57044afb2c955c6a9bbc7a736a04b014a22cbbf2183f7ed8f676ddaa`

Both modes used `publishAsync()`. The treatment changed only registered
Sync-production fields. Worker mode used exactly one production worker, one
active signer, and no receive worker. All cells had zero fallback, zero
production/publication accounting remainder, zero ownership violations, and a
drained shutdown.

## Registered Result

| Predicate | Result |
|---|---:|
| All six admissible | yes |
| Face CPU relief at least 50% | 3/3 pairs |
| Heartbeat p99 improvement at least 20% | 0/3 pairs |
| Delivery-ratio harm at most 1 percentage point | 3/3 pairs |
| Delivery-p99 harm above 10% | 2/3 pairs |
| Worker queue clean | yes |

The paired Face-production CPU relief was 57.95%, 58.15%, and 58.49%.
Heartbeat p99 instead changed by -24.17%, -9.78%, and -5.88%, where a negative
value means worker mode was slower. Delivery p99 changed by -59.39%, -7.43%,
and -15.09%. The frozen verdict is therefore `TRADE_OFF`, not
`NECESSARY_AT_TESTED_BOUNDARY`.

## Architectural Boundary

Code inspection confirms that the binary configured the inner Sync Data signer
as SHA256 and the outer V2 Sync Interest signer as HMAC. It therefore measures
a lightweight signing path. It does not establish the effect of offloading the
RSA signing path used by the intended deployment.

## Closure

Spec 139 and its campaign are frozen. No formal cell may be rerun, replaced, or
selectively discarded. The negative result is retained as evidence: with the
tested lightweight signer and 600 pps load, a single serial worker reliably
removes work from the Face thread but adds queue/handoff latency and does not
improve Face responsiveness or end-to-end tail latency.
