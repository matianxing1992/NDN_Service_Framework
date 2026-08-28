# Quickstart: Data-Centric Authorization Evaluation

## 1. Validate the registered contracts

```bash
python3 -m pytest -q tests/python/test_authorization_evaluation.py
```

Expected: the case registry and manifest schema pass; every case has an explicit
gate, terminal state, and execution count.

## 2. Run focused existing security gates

```bash
unit_tests=${NDNSF_UNIT_TESTS_BIN:-build-new-svs-20260808/unit-tests}
"${unit_tests}" --run_test=EncryptedPermissionResponse/*
"${unit_tests}" --run_test=ServiceAuthorizationTableTests/*
"${unit_tests}" --run_test=GenericDynamicApi/CryptoAndAuthorization/*
"${unit_tests}" --run_test=GenericDynamicApi/TokensAndReplay/*
```

These selectors were verified against the current `build-new-svs-20260808/unit-tests` inventory.
The case-to-selector mapping and baseline source hashes are registered in
`contracts/runtime-gates.json`; rerun the Python contract test after any runtime
or focused-test change.

## 3. Run the composed authorization smoke

```bash
python3 Experiments/run_authorization_evaluation.py \
  --mode correctness-smoke \
  --cases specs/172-data-centric-authorization-evaluation/contracts/authorization-cases.yaml \
  --output results/spec172_authorization_smoke \
  --unit-tests "${unit_tests}" \
  --security-regressions
```

Expected: every registered case reaches its expected terminal state and denied
cases record zero service handler executions; all six composed security
regressions also emit their PASS markers and are retained in the same manifest.

## 4. Run onboarding repetitions

```bash
python3 Experiments/run_authorization_evaluation.py \
  --mode onboarding \
  --repetitions 3 \
  --unit-tests "${unit_tests}" \
  --output results/spec172_onboarding
```

Expected: Provider-local hashes remain unchanged; stale epoch and refreshed epoch
are reported as separate states; the first successful invocation occurs only
after the required current policy material is installed.

## 5. Run cold/warm overhead points

```bash
python3 Experiments/run_authorization_evaluation.py \
  --mode overhead \
  --repetitions 3 \
  --duration-s 60 \
  --rate-rps 1 \
  --unit-tests "${unit_tests}" \
  --output results/spec172_authorization_overhead
```

Expected: per-run observations retain secured cold/warm latency, protected-content
bytes, crypto counters, failures, source hashes, and terminal status. The same
run executes the registered 1/10/100 provisioning operations by 1/4/16 Provider
entry matrix. These are provisioning operations, not concurrent network Users.

## 6. Confirm the paired network path in MiniNDN

```bash
sudo -n -E env PYTHONPATH="$PWD" \
  python3 Experiments/run_authorization_evaluation.py \
  --mode minindn \
  --unit-tests "${unit_tests}" \
  --output results/spec172_authorization_minindn
```

Expected: the pre-onboarding User remains at ABE-key readiness, publishes no
service request, and causes zero Provider executions; after authorization, the
unchanged Provider completes exactly one execution. The runner removes all
MiniNDN node homes and exported private-key bundles before sealing the manifest.

## 7. Analyze and admit paper results

```bash
python3 Experiments/analyze_authorization_evaluation.py \
  --input results/spec172_authorization_smoke \
  --input results/spec172_onboarding \
  --input results/spec172_authorization_overhead \
  --input results/spec172_authorization_minindn
```

Only claims reported as `SUPPORTED` may replace `TBD` cells.

## 8. Build the paper

```bash
cd docs/PAPER/named-data-network-service-framework-paper
latexmk -pdf -interaction=nonstopmode NDNSF.tex
```

Expected: successful PDF build, no unresolved citations introduced by this
feature, and no numerical authorization claim sourced from a `TBD` cell.
