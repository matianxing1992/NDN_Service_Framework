# Spec175 T021 local-suite evidence

**Date:** 2026-09-02  
**Source identity:** `evidence/t021-source-seal-current-20260902-r2.json`  
**Candidate policy:** current post-T020 source and locally linked build only.

## Acceptance runs

| Gate | Command/evidence | Result |
|---|---|---|
| C++ unit | `timeout 900s build/unit-tests --log_level=message` | PASS; 605 cases, return code 0; `t021-cpp-unit-current-20260902-r3.log` |
| Python | `scripts/run_spec175_python_gate.py --source-seal ...r2.json --output ...r4.json` | PASS; 379 passed, 1 skipped, 0 failed; native unit subprocess also returned 0 |
| C++/Python registered integration | `scripts/run_spec175_integration_gate.py --source-seal ...r2.json --healthy-repeats 1 --case-timeout-seconds 180` | PASS; all 20 registered cases I01--I20 passed in isolated child processes; `t021-integration-gate-current-20260902-r1.json` |

The C++ unit binary was rebuilt before these runs and is linked to the local
NDN-SVS and Boost-1.71-compatible NDN-CXX trees. Its recorded SHA-256 is
`sha256:21cf57c66e9ec7f465bf5f717320dbe767ce01b40a246ea18472712bfbf16b93`.
The Python gate records the host Python 3.8 extension import and reports no
runtime Transformers dependency in the deployed path.

## Diagnostic history

One earlier full unit run (`t021-cpp-unit-current-20260902-r2.log`) reported a
single intermittent failure in
`NativeProviderRuntimeIsolatesConcurrentGenerationsAndAttempts` (one future
exception and five commits). The same test passed in 100 consecutive isolated
runs, and the subsequent full run `r3` passed all 605 cases. This remains
recorded as a transient diagnostic observation; the isolated integration gate
is the qualification result and no failure was discarded.

## Scope

T021 covers the complete relevant local C++ and Python suites. Real NFD/MiniNDN
production execution remains T022, and Tiger/SIF qualification remains owned by
Spec180.
