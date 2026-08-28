# Quickstart: Spec 112 Validation

Commands are proposed until their implementation task is complete. Run from the
`ndn-service-framework` root. MiniNDN requires passwordless sudo.

## 1. Preflight

```bash
sudo -n true
df -h / /tmp
pgrep -af 'MiniNDN|minindn|NDNSF_Segmented_Response_Minindn|App_(User|Provider|ServiceController)'
codegraph status .
```

Proceed only when no other live launcher or cleanup owner exists. A stale lock
file is not sufficient ownership evidence.

## 2. Rebuild The Actual ndn-svs Candidate

```bash
cd ../ndn-svs
./waf configure --with-tests
./waf build -j"$(nproc)"
LD_LIBRARY_PATH="$PWD/build${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
  ./build/unit-tests --run_test='TestSVSPubSub/*' --log_level=test_suite
```

Record source/diff, Boost 1.71, compiler, library, and test-binary identities.
Never substitute the stale pre-change test binary.

## 3. Focused NDNSF/Python/Lifecycle Tests

```bash
cd ../ndn-service-framework
./build/unit-tests \
  --run_test='GenericDynamicApi/TargetedInvocation/*' \
  --log_level=test_suite

PYTHONPATH=pythonWrapper python3 -m unittest \
  tests.python.test_ndnsf_targeted_python_api \
  tests.python.test_spec112_segmented_response \
  tests.python.test_spec112_targeted_timeout \
  tests.python.test_spec112_nac_abe_exit
```

The Python Targeted test must use the compiled binding. A mock-only pass does not
close email defect 3.

## 4. Forced SVS Boundary Matrix

Atomically generate a unique candidate manifest, inspect the four cells, and let the
launcher set and record
`NDNSF_DISABLE_RESPONSE_LARGE_DATA_REFERENCE=1` in its isolated roles:

```bash
candidate="$(python3 Experiments/spec112_candidate_manifest.py \
  create --result-root results/spec112-segmented)"
for publish in async sync; do
  for invocation in normal targeted; do
    sync_flag=()
    if [ "$publish" = sync ]; then sync_flag=(--svs-sync-publish); fi
    sudo -n python3 Experiments/NDNSF_Segmented_Response_Minindn.py \
      --candidate-manifest "results/spec112-segmented/${candidate}/candidate-manifest.json" \
      --output-dir "results/spec112-segmented/${candidate}/boundary-${publish}-${invocation}" \
      --mode "$invocation" \
      "${sync_flag[@]}" \
      --sizes '64,4000,5000,6500,8000,16000'
  done
done
```

Every candidate/cell runs once. The launcher must reject an existing result
directory. Preserve crash, hang, timeout, and partial results.

## 5. Same-Epoch Burst And Targeted Timeout

Run one same-epoch health sequence:

```bash
sudo -n python3 Experiments/NDNSF_Segmented_Response_Minindn.py \
  --candidate-manifest "results/spec112-segmented/${candidate}/candidate-manifest.json" \
  --output-dir "results/spec112-segmented/${candidate}/burst-async-normal" \
  --mode normal \
  --sizes '8000x80,64x10,4000x12'
```

Then establish Targeted state, stop the Provider through the declared fault
profile, and verify exactly one timeout callback by `timeout_ms + 500 ms`:

```bash
sudo -n python3 Experiments/NDNSF_Segmented_Response_Minindn.py \
  --candidate-manifest "results/spec112-segmented/${candidate}/candidate-manifest.json" \
  --output-dir "results/spec112-segmented/${candidate}/targeted-degraded-timeout" \
  --mode targeted \
  --sizes '64' \
  --fault-profile degraded-provider-after-targeted-bootstrap \
  --timeout-ms 4000
```

Expected candidate artifacts:

```text
results/spec112-segmented/<candidate>/
├── candidate-manifest.json
├── campaign-summary.json
├── campaign-cells.csv
└── <cell-id>/
    ├── controller.log
    ├── provider.log
    ├── user.log
    ├── nfd-*.log
    └── cell-summary.json
```

## 6. Completion Checks

```bash
.specify/scripts/bash/check-prerequisites.sh --json --require-tasks --include-tasks
git diff --check
```

Spec 112 is not complete until all five email defects have focused evidence and
the final audit finds no unrelated API, transport, experiment, or DI work.
