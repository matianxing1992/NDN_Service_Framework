# T011 local Y-N P-only live closure — 2026-09-05 r38

## Disposition

`PASS` for the focused Y-N-P live subcase only. The maintained
`_run_live_case_once` path crossed Controller `PUBPARAMS` readiness, ran the
real P negative path, emitted its fail-closed marker, and cleaned up the
child set. This does not close the complete Y-N matrix.

## Run identity and result

- candidate: current local Spec180 YOLO candidate package and native provider
  binary built in `build-system-j2`;
- input bundle: local Y-N input bundle `r115`;
- source under test: Controller startup drains use
  `processEvents(1000ms, keepRunning=true)`;
- command: the maintained `_run_live_case_once(..., subcase="Y-N-P")` helper
  under `unshare -Urnm`, with the normal local candidate/config environment;
- runner exit: `0`;
- marker: `SPEC180_SUBCASE_RESULT status=PASS subcase=Y-N-P`.

The raw output and state are retained in an ignored workspace temporary run
identified as `spec180-yolo-y-n-current-20260905-r38-p-only`.

## Evidence boundary

The run reached the Controller readiness barrier and therefore distinguished
the P-only ACK provenance path from the startup failure seen in r36. It is
useful focused evidence, but it is not a substitute for the complete Y-N
matrix, T014 convergence, or any SIF/Tiger qualification gate. No SIF or
TigerCluster command was run.

## Closure of the preceding preflight failure

The r37 P-only preflight failure was caused solely by omitting
`unshare -Urnm`; r38 used the required boundary and crossed the preflight.
The active Spec180 blocker remains the r36 matrix result until a fresh full
matrix reaches the P subcase and all child cleanup checks pass.
