# Python Final Gate

Date: 2026-07-14  
Verdict: **PASS**

Command:

```bash
python3 -m unittest discover -s tests/python -p 'test_ndnsf_di_*.py'
```

Post-remediation result: **401 tests, 401 passed, 0 failed, 1
environment-conditioned skip**, 26.473 seconds (27 wall seconds). Raw log:
`/tmp/spec111-final-r2/python.log`, SHA-256
`a4fd071b4c66875f70d3207f2b67966bf0a816e054a16ad9c3175ec021f43c67`.

An earlier full run found two Tk dry-run failures. Moving GUI ownership one
directory deeper had left `repo_root()` at the old parent depth, so the worker
could not open the NativeTracer MiniNDN harness. Repository-root discovery was
made marker-based; the two focused tests and then all 399 tests passed.
