# T011 local Y-N live attempt — 2026-09-05 r39

## Disposition

`UNQUALIFIED`: the fresh full local NFD Y-N matrix crossed the Controller
readiness boundary for six subcases, but Y-N-I did not. No SIF or
TigerCluster command was run for r39.

## Run identity

- candidate: current local Spec180 YOLO candidate package and native provider
  binary built in `build-system-j2`;
- input bundle: local Y-N input bundle `r115`;
- source under test: Controller startup drains use
  `processEvents(1000ms, keepRunning=true)` before and after Controller
  registrations;
- command: `unshare -Urnm python3 Experiments/NDNSF_DI_YoloAckDriven_Minindn.py
  --case Y-N`;
- runner exit: `2`;
- ambient diagnostic: `*** Error setting resource limits. Mininet's
  performance may be affected.`

The raw output and state are retained in an ignored workspace temporary run
identified as `spec180-yolo-y-n-current-20260905-r39`.

## Structured result

```text
SPEC180_SUBCASE_RESULT status=PASS subcase=Y-N-O
SPEC180_SUBCASE_RESULT status=PASS subcase=Y-N-C
SPEC180_SUBCASE_RESULT status=PASS subcase=Y-N-P
SPEC180_SUBCASE_RESULT status=UNQUALIFIED subcase=Y-N-I
SPEC180_SUBCASE_RESULT status=PASS subcase=Y-N-R
SPEC180_SUBCASE_RESULT status=PASS subcase=Y-N-E
SPEC180_SUBCASE_RESULT status=PASS subcase=Y-N-L
SPEC180_CASE_RESULT status=UNQUALIFIED error=Y_N_MATRIX_INCOMPLETE:Y-N-I
```

## First failing boundary

For Y-N-I, the Controller log reaches the six `NDNSF/...` registration
requests, but the Unix Face closes immediately after the first connection is
created. The log contains no `registered prefix: /example/controller` and no
`setting InterestFilter: /example/controller/PUBPARAMS`; Python eventually
raises `RuntimeError: ServiceController readiness timeout`.

The NFD log shows face 300 removed and face 301 receiving only the six
Controller routes. This is the same startup/transport boundary seen in r36
Y-N-P, while r38 showed that the P-only protocol path succeeds when startup
does not hit the race. Y-N-I therefore never reached its intended
`PROVIDER_EXECUTION_STARTED` boundary and cannot be counted as a protocol
negative result.

## Invalidation and next action

r39 does not close the Y-N matrix or advance T014/T015. It confirms that the
`keepRunning` change reduces but does not eliminate the startup race. Before
another live retry, inspect a temporary startup trace that distinguishes the
two Controller drain calls and the transport close. Remove that temporary
instrumentation after diagnosis; do not run SIF or Tiger while the local live
readiness barrier is unresolved.
