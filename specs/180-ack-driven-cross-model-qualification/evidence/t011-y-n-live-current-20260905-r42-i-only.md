# T011 current-source Y-N-I focused run r42

Date: 2026-09-05 (local)

## Result

`PASS` for the focused current-source Y-N-I diagnostic. The maintained
helper emitted:

```text
SPEC180_SUBCASE_RESULT status=PASS subcase=Y-N-I
```

The process exited 0. No SIF or Tiger execution was performed.

## Artifact binding

The live Python import resolved to:

```text
pythonWrapper/ndnsf/_ndnsf.cpython-38-x86_64-linux-gnu.so
```

The extension RUNPATH resolved
`libndn-service-framework.so.0.1.0` from the local `build-system-j2`
directory. The source and loaded build artifacts used for this run were
hashed after the rebuild as follows:

```text
ServiceController.cpp:       9dc607e23367515d388141a58ddac83ad5b349d009d8aeb4e32fe7f4b89049de
libndn-service-framework.so: d4133ff5dcf5256204d47083a2f4cd1c29ff841b613917b6c24752908fa6c4dd
_ndnsf.cpython-38...so:      af94a044f8c83cd2b7aeea14962affa2e0ca5895e72f850c0c2da9ecfc321f90
```

The linked framework library contains the current diagnostic marker and the
Controller log contains the complete sequence:

1. `pre-drain begin`;
2. successful `registered prefix: /example/controller`;
3. `setting InterestFilter: /example/controller/PUBPARAMS`;
4. `pre-drain end`;
5. controller-registration and registration-drain phases;
6. readiness probe Interest with `CanBePrefix` and `MustBeFresh`; and
7. a successful `PUBPARAMS` response.

This closes the r41 artifact-identity boundary and the r39 focused
Y-N-I-before-readiness occurrence. The Y-N-I lifecycle reached
`ACK_CLOSED`, `GRAPH_READY`, `PLACEMENT_DECISION`, `ARTIFACTS_READY`,
`PLAN_SEALED`, `SELECTION_COMMITTED`, and `PROVIDER_EXECUTION_STARTED`; the
native provider emitted the expected `NON_INGRESS_INPUT_REJECTED` reason.

## Evidence boundary

This is focused T011 repair evidence, not the T014 convergence verdict and
not the complete Y-N matrix. The temporary diagnostic marker must be removed
and the framework library and Python extension rebuilt before the final live
matrix is run. The raw run remains in the ignored workspace temporary run
identified by `spec180-yolo-y-n-current-20260905-r42-i-only`.
