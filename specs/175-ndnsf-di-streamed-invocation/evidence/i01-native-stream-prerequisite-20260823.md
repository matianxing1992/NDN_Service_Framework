# Native I01 prerequisite: tiny ONNX through NDNSF Core (2026-08-23)

`Spec175InvocationStream/NativeTinyOnnxStreamRunsPersistentState` runs the
checked-in one-role tiny causal model inside a real `ServiceProvider` streamed
handler. The request uses the normal encrypted Request path; the event Data and
terminal End/Response use the existing Core stream transport and exact-Interest
delivery. The Provider executes eight incremental ORT steps, reusing the
attention-KV, recurrent, and convolution state outputs from the prior step.

The observed application event sequence is `4,5,6,7,8,9,10,2`, followed by one
`Eos` End and the final `native-onnx` Response. The stream option is
`maxEvents=9` because the signed End occupies the ninth cursor (`maxEvents`
counts both application events and End).

Reproduction:

```text
./waf build --target=integration-tests -j2
build/integration-tests --run_test=Spec175InvocationStream/NativeTinyOnnxStreamRunsPersistentState --log_level=test_suite
```

This is an I01-level prerequisite, not a formal G2 registration: it still uses
the generic Core handler rather than the model/task-first multi-Provider
placement and therefore does not establish I02-I15 or the G2 process manifest.
