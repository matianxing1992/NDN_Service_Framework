# Native Final Gate

Date: 2026-07-14  
Verdict: **PASS**

The built `build/unit-tests` binary ran its complete suite, which includes the
frozen plan, Provider worker, dependency scheduler, exact-cache/KV,
execution-attempt, artifact materialization, resource probe, Qwen generation
and distributed-consistency coverage. Result: **267 cases, 0 errors** in 75
wall seconds. Raw log: `/tmp/spec111-final-r2/native.log`, SHA-256
`fa3e3bd046914b44325effe33afc7747321a2a137e6d381fe9095a0a63d31621`.

Three generated-plan smokes reported their existing `NDNSF_DI_NATIVE_PLAN_JSON
not set` no-assertion skip marker; generated JSON behavior is covered by the
non-environmental plan cases in the same binary.
