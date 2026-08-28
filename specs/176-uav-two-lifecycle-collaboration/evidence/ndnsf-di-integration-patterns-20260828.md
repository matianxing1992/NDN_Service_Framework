# NDNSF-DI integration-test patterns used by Spec176

This note records which existing NDNSF-DI integration tests are used as
organizational references for the UAV acceptance gates. The references are
test-structure patterns only; Spec176 keeps application-owned UAV payloads and
does not import DI payload types, dependencies, or Core wire changes.

| NDNSF-DI reference | Reused pattern | Spec176 application |
| --- | --- | --- |
| `tests/integration-tests/ndnsf-integration-fixture.hpp/.cpp` | Build a complete in-process environment (faces, identities, SVS, policy, permissions), bootstrap it once, then isolate each request and inject faults deterministically. | CPU/in-process UAV fixture starts the MissionSession and stream bindings before finite incident jobs; each incident has its own request/attempt lineage. |
| `tests/integration-tests/ndnsf-di-core-flow.t.cpp` | Assert the production ingress, ACK closure, selection, assignment, exact named fetch, role split, and explicit failure stages. | UAV flow asserts ACK_CLOSED -> committed plan -> Selection -> exact producer Data fetch -> one terminal DetectorReporter result, with failure-stage evidence. |
| `tests/integration-tests/ndnsf-data-v1-svs-flow.t.cpp` | Use manifests, exact segment Interests, mapping/recovery checks, replay checks, and reassembly assertions. | UAV named-evidence test uses immutable producer names, exact non-prefix Interests, validator-backed Data verification, digest binding, and second-consumer retrieval. |
| `tests/integration-tests/invocation-stream-flow.t.cpp` | Exercise lifecycle, retry/reorder/duplicate/tamper/cancel/capacity behavior with bounded waits and machine-readable markers. | UAV tests keep streams independent from finite request failures and cover stale callbacks, evidence tamper/expiry, bounded queue pressure, and cancellation. |
| `scripts/run_spec175_python_gate.py` | Record explicit commands, return codes, artifact hashes, and classify historical diagnostics separately from acceptance evidence. | The PX4 runner emits a candidate-bound manifest, readiness barriers, stage-correlated logs, hashes, and a non-success status for missing SITL prerequisites. |

## Boundary and gate

The references do not authorize a direct DI dependency in `NDNSF-UAV-APP`.
Spec176's validation order remains:

1. unit tests;
2. CPU/in-process integration tests;
3. real multi-process MiniNDN;
4. PX4 SITL.

A later gate cannot mask an earlier failure. The current unit, CPU, MiniNDN,
and candidate-bound PX4/jMAVSim evidence is accepted. The official wrapper
run `results/spec176-rootless-sitl-r14-20260828/summary.json` returned code 0
with all seven required stage markers; this closes the SITL acceptance gate but
does not authorize promotion or hardware-flight claims.
