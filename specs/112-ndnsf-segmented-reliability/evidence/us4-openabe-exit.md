# US4 OpenABE/RELIC Process Exit Gate

## Final Disposition

**No additional NAC-ABE product-source change was made in US4.** The current
working implementation already owns OpenABE operations on one process-wide
thread, keeps that executor alive until OS process reclamation, and avoids
destructor-time `ShutdownOpenABE()`. The 10-cycle decision baseline passed, so
Spec 112 followed its stop rule and did not add another teardown mechanism.

## 100-Cycle Acceptance

Command:

```text
python3 tests/python/test_spec112_nac_abe_exit.py \
  --run-campaign \
  --cycles 100 \
  --output results/spec112-nac-abe-lifecycle/final-current-100cycles-20260715.json \
  --timeout-s 5
```

Evidence JSON:
`results/spec112-nac-abe-lifecycle/final-current-100cycles-20260715.json`
(`0386edec7b94cd3967c3da778ae4b33fbaa18135d9c1941ed87806c4cd9e4f57`).

Test binary:
`../NAC-ABE/build-tests/tests/unit-tests`
(`90bdc8ff8eecc63442a84c09fce35c383447ac947f95387109475d27ecdfb3d9`).

| Role | Exits | Normal | Controlled | Successful | SIGSEGV | SIGABRT |
|---|---:|---:|---:|---:|---:|---:|
| Controller | 100 | 50 | 50 | 100 | 0 | 0 |
| Provider | 100 | 50 | 50 | 100 | 0 | 0 |
| User | 100 | 50 | 50 | 100 | 0 | 0 |
| **Total** | **300** | **150** | **150** | **300** | **0** | **0** |

Additional totals: zero timeout, zero nonzero exit, and zero sanitizer marker.
The binary was not sanitizer-instrumented; that limitation is machine-recorded
for every exit and is not represented as sanitizer coverage. Total campaign
time was 19.165 seconds.

This closes the reported current-process exit failure for the tested x86-64
Ubuntu build. It does not claim that the reporter's older aarch64/NixOS binary
was tested locally.
