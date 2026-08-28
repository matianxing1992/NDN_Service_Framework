# US4 Current-Code Lifecycle Decision Baseline

## Code Reality Before The Decision Gate

The current dirty NAC-ABE source already contained a process-wide
`OpenAbeExecutor` that initializes and uses OpenABE on one dedicated thread.
`ABESupport::getInstance()` and the executor intentionally remain alive until
process exit, and `ABESupport::~ABESupport()` does not call
`ShutdownOpenABE()`. These changes predate the US4 lifecycle probe in this
workstream and were preserved as current source reality.

Spec 112 therefore tested that implementation before making any additional
teardown edit.

## Executed Probe

`TestAbeSupport/ProcessLifecycleProbe` performs real CP-ABE setup, private-key
generation, encryption, and decryption for a role-labelled Controller,
Provider, or User process. Half of the processes exit normally; half install a
minimal SIGINT handler, acknowledge readiness after cryptographic use, receive
SIGINT, and then return through normal C++ process exit.

Driver schema records role, shutdown cause, exit code, terminating signal,
timeout, cryptographic-use marker, and sanitizer-marker scan for every process.
The current build is not ASan/UBSan-instrumented, so `sanitizerEnabled=false` is
recorded explicitly rather than claiming sanitizer coverage.

Command:

```text
python3 tests/python/test_spec112_nac_abe_exit.py \
  --run-campaign \
  --cycles 10 \
  --output results/spec112-nac-abe-lifecycle/pre-fix-current-20260715.json \
  --timeout-s 5
```

Result:

| Metric | Value |
|---|---:|
| Cycles | 10 |
| Role exits | 30 |
| Normal / controlled | 15 / 15 |
| Successful exits | 30 |
| Failures / timeouts | 0 / 0 |
| SIGSEGV / SIGABRT | 0 / 0 |
| Sanitizer markers | 0 |
| Elapsed | 2.050 s |

Binary:
`../NAC-ABE/build-tests/tests/unit-tests`
(`90bdc8ff8eecc63442a84c09fce35c383447ac947f95387109475d27ecdfb3d9`).

## Decision Gate

Disposition: **no additional NAC-ABE product-source change**.

The current initialized-process baseline passed. T035 therefore forbids a new
guarded `ShutdownOpenABE`, destructor rewrite, or other speculative lifetime
change. The unchanged implementation proceeds to the 100-cycle acceptance
gate.
