# Final Validation Evidence

## Root Cause And Implementation

The original lifecycle abort was enabled by shutdown ordering: the Ground
Station checked and joined workers before quiescing the Face thread that could
still create them. The implementation now quiesces and joins producers first,
owns and joins the auto-MAVLink thread, and runs lightweight `ServiceUser`
callbacks on the Face thread with `setHandlerThreads(0)`.

Generic Targeted and UAV command diagnostics expose correlated terminal stages
without logging payloads, tokens, certificates, credentials, or key material.
The parser independently rejects both known abort signatures and a clean exit
with a nonterminal command attempt. After concurrent automation sessions collided
on output paths, the campaign also gained an exclusive directory lock and a
fresh-output guard.

## Reproduction

```bash
python3 Experiments/NDNSF_UAV_Stream_Control_Isolation_Campaign.py \
  --out results/spec097-uav-targeted-control-loss00-current-final \
  --workload-modes control-only --runs 5 --loss-percent 0 \
  --auto-stop-seconds 60

python3 Experiments/NDNSF_UAV_Stream_Control_Isolation_Campaign.py \
  --out results/spec097-uav-targeted-control-loss05-current-final \
  --workload-modes control-only --runs 5 --loss-percent 5 \
  --auto-stop-seconds 60
```

The 5% measurement ran under a unique exclusive path to isolate stale
executors, then moved intact to the canonical path after completion.
Interrupted/concurrent-launch directories remain separate and are excluded
from the ten-run matrix.

## Results

| Loss | Runs | Process completion | Control completion | Lifecycle aborts | Unterminated attempts |
|---|---:|---:|---:|---:|---:|
| 0% | 5 | 5/5 | 5/5 | 0/5 | 0/5 |
| 5% | 5 | 1/5 | 1/5 | 0/5 | 0/5 |

At 0%, Arm, Takeoff, Land, and Emergency Stop reached `response` in every run.
At 5%, latest-stage totals were Arm 4 `response`/1 `blocked`, Takeoff 1
`response`/4 `blocked`, Land 4 `response`/1 `blocked`, and Emergency Stop 5
`response`.

All ten runs contain neither `terminate called without an active exception`
nor `__pthread_tpp_change_priority`. Every attempted command ends at
`response`, `timeout`, `blocked`, or `busy`. No failed repetition was replaced,
and no timeout, retry, protocol, or security behavior was tuned.

## Verification And Residual Risk

- 216/216 C++ unit tests passed before the final matrix.
- The focused campaign suite passes 14/14 after the output guards.
- Strict Spec Kit structural audit passes with complete traceability.
- CodeGraph is synchronized; proposal and paper paths are unchanged.

Lifecycle acceptance is supported by zero abort markers across ten
current-revision runs. Network reliability is not fixed or claimed: 5% loss
still completes only 1/5 control runs, primarily because Takeoff is blocked
after preceding loss-dependent state does not satisfy admission. That boundary
is the next measured feature; it must not reopen this lifecycle fix or add
retries by default.
