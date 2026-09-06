# T011 local Y-N live attempt — 2026-09-05 r36

## Disposition

`UNQUALIFIED`: the fresh local NFD Y-N matrix reached the negative-path
driver for six subcases, but Y-N-P did not cross the Controller
`PUBPARAMS` readiness boundary. This is a startup/transport failure, not an
ACK provenance result. No SIF or TigerCluster command was run for r36.

## Run identity

- candidate: current local Spec180 YOLO candidate package and native provider
  binary built in `build-system-j2`;
- input bundle: local Y-N input bundle `r115`;
- source change under test: Controller startup drains use
  `processEvents(1000ms, keepRunning=true)` before and after the Controller
  registrations;
- command: `unshare -Urnm python3 Experiments/NDNSF_DI_YoloAckDriven_Minindn.py
  --case Y-N`;
- runner exit: `2`;
- ambient diagnostic: `*** Error setting resource limits. Mininet's
  performance may be affected.`

The raw output directory and console log are retained in an ignored workspace
temporary run identified as `spec180-yolo-y-n-current-20260905-r36`.

## Structured result

The runner emitted:

```text
SPEC180_SUBCASE_RESULT status=PASS subcase=Y-N-O
SPEC180_SUBCASE_RESULT status=PASS subcase=Y-N-C
SPEC180_SUBCASE_RESULT status=UNQUALIFIED subcase=Y-N-P
SPEC180_SUBCASE_RESULT status=PASS subcase=Y-N-R
SPEC180_SUBCASE_RESULT status=PASS subcase=Y-N-I
SPEC180_SUBCASE_RESULT status=PASS subcase=Y-N-E
SPEC180_SUBCASE_RESULT status=PASS subcase=Y-N-L
SPEC180_CASE_RESULT status=UNQUALIFIED error=Y_N_MATRIX_INCOMPLETE:Y-N-P
```

## First failing boundary

The Y-N-P Controller log shows the same generated controller policy and trust
schema as Y-N-E. It logs registration of the two certificate prefixes, the
root `/example/controller`, and the six Controller filters. On the first Face
connection it sends the certificate registration requests and then closes the
Unix transport before the root registration request is sent. A reconnect
serves only the six `NDNSF/...` registrations:

- no `registered prefix: /example/controller`;
- no `setting InterestFilter: /example/controller/PUBPARAMS`;
- Python eventually raises `RuntimeError: ServiceController readiness timeout`.

The corresponding NFD log shows the first Controller face removed and a new
face receiving only the six NDNSF routes. Therefore the run never reached the
Y-N-P ACK-closed boundary and cannot prove or disprove the intended
`ACK_PROVENANCE_REJECTED` disposition.

## Comparison and invalidation

r36 is a useful repair signal: Y-N-E now reaches its intended negative marker,
while Y-N-P remains unqualified. The successful subcases do not close the
matrix, and the previous r35 record remains open as historical evidence. No
candidate is promoted and no downstream T014/T015/SIF/Tiger gate advances.

## Next allowed action

Preserve r36, inspect its P controller/NFD logs before retrying, and run one
fresh local Y-N attempt after a focused startup repair or a clearly isolated
timing hypothesis. The next successful closure must include the P negative
marker, all child exit statuses, and the normal cleanup contract. Do not treat
the resource-limit diagnostic or a readiness timeout as an ACK result.
