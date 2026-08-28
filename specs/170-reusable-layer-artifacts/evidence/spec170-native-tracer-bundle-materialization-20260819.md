# Spec170 NativeTracer bundle materialization repair — 2026-08-19

## Finding

The real host MiniNDN NativeTracer path launched Providers with the expected
`cd <policy-bundle> && exec ...` command, but `plan_tracer.py` copied only the
manifest and plan metadata into the generated bundle.  The manifest still
referenced relative files such as `artifacts/qwen-native-tracer-backbone.onnx`,
while `policy-bundle/artifacts/` did not exist.  All four Providers therefore
failed during provisioning before the request lifecycle:

```text
NDNSF_DI_NATIVE_PROVIDER_PROVISION_FAILED
Load model artifacts/qwen-native-tracer-*.onnx failed. File doesn't exist
```

This was a bundle-materialization defect, not a SIF, ORT, MiniNDN, or protocol
failure.  The earlier `cd "$BUNDLE"` rule remains necessary, but it is not
sufficient unless the generated bundle contains every referenced relative
artifact.

## Repair

`examples/python/NDNSF-DistributedInference/native_di_tracer/plan_tracer.py`
now:

1. verifies each source artifact and its SHA-256 sidecar;
2. copies relative artifacts and sidecars below the generated bundle root;
3. rejects path traversal and missing source files; and
4. requires the copied artifact and sidecar to pass the same hash check during
   `validate_bundle()`.

The regression is covered by
`tests/python/test_ndnsf_di_runtime_aware_campaign.py::test_plan_tracer_materializes_relative_artifacts_into_bundle`.

## Real rerun

Command (host C++ build, real MiniNDN, CPU ONNX Runtime; one request):

```bash
SPEC170_ORT_LIBRARY_PATH=/home/tianxing/.local/lib/python3.8/site-packages/onnxruntime/capi \
LD_LIBRARY_PATH="$SPEC170_ORT_LIBRARY_PATH:$LD_LIBRARY_PATH" \
sudo -n timeout 100s env "LD_LIBRARY_PATH=$LD_LIBRARY_PATH" \
python3 Experiments/NDNSF_DI_NativeTracer_Minindn.py \
  --full-network --core-trace --assignment default \
  --requests 1 --concurrency 1 --provider-check-timeout 30 \
  --skip-provider-pair-telemetry-probe \
  --overload-fast-fail-timeout-ms 15000 \
  --out /tmp/ndnsf-native-rerun-wpjNzu
```

Observed result:

| Evidence | Result |
|---|---|
| runner classification/mode | `onnxruntime-cpu` / `onnxruntime-cpu` |
| Provider provisioning | 4/4 ready; `loadCompleted=true`, `warmupCompleted=true`, `cpuFallbackUsed=false` |
| User request | 1 submitted, 1 successful, 0 failed, 0 timeouts |
| Request timing | mean/p50/p95 `293.8918 ms` (single sample; descriptive only) |
| Dependency edges | 4 expected, 4 published, 4 fetched; 8 successful object events |
| Integrity | no missing publication/fetch, name mismatch, or empty payload |
| Lifecycle | ACK candidates → Selection → Provider execution → Response succeeded |
| Admission | disabled (`maxQueue=-1`, `maxActiveWorkers=-1`) |
| negative ACKs | 0 |

The generated bundle contained all four ONNX files and their sidecars.  This
closes the pre-Selection provisioning defect for the current host runner.  It
does not qualify the dirty worktree for Tiger or replace the source-sealed SIF
gate: the Python runner change requires a new candidate/source seal before a
new SIF is promoted.

## Workload cleanup repair

The first exact-SIF D0/D1 rerun exposed a separate harness defect: D0 launched
the Provider through a backgrounded shell function whose nested subshell used
`exec`; cleanup killed the function shell but left four Provider executables
running.  The D0 workload now changes directory in the background function
itself and uses `exec env ... di-native-provider`, so the PID stored in `PIDS`
is the serving executable.  D1 uses the same direct-exec shape.  The static
contract test rejects either workload if this guarantee is removed.

After the repair, exact r23 SIF D0 and D1 each passed their real
Controller/Provider/User lifecycle gate (`1 passed` each, about 10.5 seconds)
and the post-test process census found zero remaining
`/opt/ndnsf-di/current/bin/di-native-provider` processes.  This cleanup result
is a harness invariant; it does not change r23's sealed-source boundary.
