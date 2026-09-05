# T011 current-source Y-N matrix attempt r35 (2026-09-05)

## Disposition

`UNQUALIFIED`. This is a retained failure record, not qualification evidence.
The run used the current source tree and the rebuilt `build-system-j2`
`di-native-provider`, but two negative subcases stopped at Controller
`PUBPARAMS` readiness before their ACK-disposition mutation could be tested.

No SIF or Tiger action was performed for this attempt.

## Run identity

- Runner: `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-N`
- Output: ignored workspace temporary run `spec180-yolo-y-n-current-20260905-r35/`
- Console log: ignored workspace temporary console log with the same run-id
- Inputs: ignored workspace temporary input set `spec180-yolo-y-n-inputs-r115/`
- Candidate: ignored workspace temporary package `spec180-yolo-candidate-current/`
- Native binary: `build-system-j2/examples/di-native-provider`
- Runner exit: `2`

The runner also reported `*** Error setting resource limits. Mininet's
performance may be affected.` This is retained as an environment diagnostic;
the controlling per-subcase failure below is the Controller readiness timeout.

## Subcase result

| Subcase | Result | Boundary/reason |
| --- | --- | --- |
| Y-N-O | `PASS` | control permutation completed |
| Y-N-C | `PASS` | control mutation completed |
| Y-N-R | `PASS` | `PLAN_SEALED`, `ROLE_KIND_REJECTED` |
| Y-N-I | `PASS` | `PROVIDER_EXECUTION_STARTED`, `NON_INGRESS_INPUT_REJECTED` |
| Y-N-L | `PASS` | `EVIDENCE_ACCEPTANCE`, `REDACTION_REJECTED` |
| Y-N-P | `UNQUALIFIED` | `FAIL_CLOSED_NOT_PROVEN`; Controller readiness timeout |
| Y-N-E | `UNQUALIFIED` | `FAIL_CLOSED_NOT_PROVEN`; Controller readiness timeout |

The terminal summary was:

```text
SPEC180_CASE_RESULT status=UNQUALIFIED error=Y_N_MATRIX_INCOMPLETE:Y-N-P,Y-N-E
```

## First failing boundary

For both `Y-N-P` and `Y-N-E`, the Controller log contains:

```text
registering prefix: /example/controller
registering prefix: /example/controller/NDNSF/SERVICEACCESS
registering prefix: /example/controller/NDNSF/SERVICEPROVISION
registering prefix: /example/controller/NDNSF/PERMISSIONS/USER
registering prefix: /example/controller/NDNSF/PERMISSIONS/PROVIDER
registering prefix: /example/controller/NDNSF/POLICY-MANIFEST
registering prefix: /example/controller/NDNSF/CERTBOOTSTRAP
```

The same logs do not contain a successful
`registered prefix: /example/controller` or
`setting InterestFilter: /example/controller/PUBPARAMS` before:

```text
RuntimeError: ServiceController readiness timeout
```

The corresponding structured records are:

```text
ignored workspace temporary/spec180-yolo-y-n-current-20260905-r35/subcases/Y-N-P/subcase-result.json
ignored workspace temporary/spec180-yolo-y-n-current-20260905-r35/subcases/Y-N-E/subcase-result.json
ignored workspace temporary/spec180-yolo-y-n-current-20260905-r35/subcases/Y-N-P/controller.log
ignored workspace temporary/spec180-yolo-y-n-current-20260905-r35/subcases/Y-N-E/controller.log
```

This is a startup/event-loop or local NFD registration boundary. It is not
evidence that the P or E ACK mutation was accepted or rejected, because no
negative-case terminal protocol marker was reached.

## Effect and next gate

- Do not mark T011 complete from r35.
- Do not use r35 to open T014, T015, SIF, or Tiger gates.
- Reproduce the Controller registration/readiness failure with a fresh run,
  inspecting the newest Controller and NFD logs before changing source.
- Only a complete Y-N matrix with all seven subcases at their specified
  boundaries can close the live-negative portion of T011.
