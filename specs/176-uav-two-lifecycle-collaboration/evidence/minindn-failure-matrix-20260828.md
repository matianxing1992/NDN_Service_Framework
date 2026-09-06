# Spec 176 MiniNDN failure-matrix evidence

**Run date:** 2026-08-28
**Candidate:** `UAV-Experimental` after the local UAV application build
**Topology:** `gs`, `scout-a`, `scout-b`, `compute` with the registered
`topology.conf` and shared PIB/TPM
**Runner:** `examples/ndnsf/uav-collaboration/minindn_failure_matrix.py`
**Command:**

```text
python3 examples/ndnsf/uav-collaboration/minindn_failure_matrix.py \
  --run --window-seconds 8 \
  --output /tmp/spec176-failure-matrix4.FAmMYX
```

The matrix runner passed `--ack-timeout-ms 1500 --timeout-ms 5000` to every
Ground Station process.  The larger finite ACK window prevents a cold identity
and permission bootstrap from being mistaken for provider failure; it does
not change the bounded request semantics.  Each case ran in an isolated
rootless user/network namespace with `unshare --user --map-root-user --mount
--net`.

## Result

The final `summary.json` reports `status: PASS` for all ten cases:

| Case | Expected terminal state | Observed result | Safety evidence |
| --- | --- | --- | --- |
| `no-feasible-detector` | FAILED | `GS_INCIDENT_COLLABORATION_EXIT ok=false` | no fallback execution |
| `compute-uav-loss` | FAILED | `ok=false` | compute process loss; no terminal success |
| `fallback-disabled` | FAILED | `ok=false` | no implicit Ground Station fallback |
| `fallback-enabled` | SUCCEEDED | `ok=true` | two `GS_FALLBACK_EXECUTED` events; selected provider `/example/uav/gs` |
| `evidence-expiry` | FAILED | `ok=false` | named Data freshness/verification path does not complete |
| `evidence-tamper` | FAILED | `ok=false` | no Data was accepted/verified |
| `selected-provider-loss` | FAILED | `ok=false` | selected compute provider is killed; no silent owner change |
| `delayed-older-result` | FAILED | `ok=false` | `RESPONSE_ATTEMPT_REJECTED` for `attempt-older` |
| `terminal-delivery-loss` | FAILED | `ok=false` | terminal delivery loss reaches bounded failure |
| `bounded-queue-overload` | FAILED | `ok=false` | detector work is rejected before unbounded queueing |

The successful fallback case recorded two exact evidence Interest/Data fetches,
two `COLLAB_DATA_VERIFIED` events, and two explicit degraded CPU executions.
The delayed-result case recorded one exact fetch/verification and rejected the
response whose `attempt_id` did not match the current request.  The tamper case
issued Interests but recorded no received or verified Data.  Failure cases did
not claim a service success merely because the launcher itself completed.

## Reproducibility hashes

These hashes bind the summary and representative logs to the run directory
above:

```text
b92c8bdb35b9369a592d1dd0c19e142d2c78e2fd49a7e6697da1654f95e60dbc  manifest.json
3cf2e3bed2f5a9bc1194301b812899b8395ecda375b7f46e8a2fe40cb86c650c  summary.json
0b05fbfd186b3ceab3c16d7dc90c9675ae613ae06eccde9a7457e2734cab4a64  fallback-enabled/ground-station.log
582a5a5d31325d207c15b4cb685109caa503349de70750d26f17d2f1d559178c  delayed-older-result/ground-station.log
ca41a1b3ae2992133f53963726dbd06ade80189ebf031c442fad7cb6894e7bc5  evidence-tamper/ground-station.log
```

This is failure-matrix evidence only.  It does not claim multi-segment stream
coverage or PX4 SITL coverage; those remain separate Spec 176 gates.

## Post-selector candidate rerun

Date: 2026-08-28
Output: `/tmp/spec176-selector-failure2.RQ4TB7`

The same matrix was rerun after the Ground Station capability selector and
controller identity-path fix were built. All ten cases passed their expected
terminal-state/invariant checks:

```text
no-feasible-detector    PASS  FAILED
compute-uav-loss        PASS  FAILED
fallback-disabled       PASS  FAILED
fallback-enabled       PASS  SUCCEEDED (GS_FALLBACK_EXECUTED)
evidence-expiry         PASS  FAILED
evidence-tamper         PASS  FAILED
selected-provider-loss  PASS  FAILED
delayed-older-result    PASS  FAILED
terminal-delivery-loss  PASS  FAILED
bounded-queue-overload  PASS  FAILED
status=PASS
```

The matrix summary and frozen manifest hashes are:

```text
b92c8bdb35b9369a592d1dd0c19e142d2c78e2fd49a7e6697da1654f95e60dbc  manifest.json
5280594b2cbf72b118c5ea8abff0ece65bc35cb183025580396667c1feaacd71  summary.json
```

Failure cases intentionally return code 1 from the per-case launcher because
the expected collaboration terminal state is `FAILED`; the matrix analyzer
classifies the observed state and reports PASS. This rerun closes T019 for the
current selector candidate; multi-segment adverse-data and PX4 SITL gates remain
separate.
