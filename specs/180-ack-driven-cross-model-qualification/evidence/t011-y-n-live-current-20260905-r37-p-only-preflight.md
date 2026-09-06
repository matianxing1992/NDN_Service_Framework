# T011 diagnostic preflight failure — 2026-09-05 r37 P-only

## Disposition

`UNQUALIFIED` preflight only. The focused P-only diagnostic did not create an
NFD network or start a child protocol process because the command omitted the
user-namespace root boundary required by MiniNDN. It is not evidence about
Controller readiness or ACK provenance.

## Exact failure

The maintained `_run_live_case_once(..., subcase="Y-N-P")` helper was invoked
from Python without the surrounding `unshare -Urnm` command. It terminated
with:

```text
*** Mininet must run as root.
```

The process exit status was `1`. A unique ignored workspace temporary output
and state directory was created for the attempt; no SIF or TigerCluster work
was run.

## Effect and next action

This attempt does not change the r36 Controller/transport diagnosis and does
not advance any Spec180 gate. Retry the same P-only helper through the normal
`unshare -Urnm` boundary in a new run directory, then inspect its Controller
and NFD logs before changing source.
