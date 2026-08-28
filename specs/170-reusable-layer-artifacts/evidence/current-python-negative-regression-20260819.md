# Current-source Python negative regression (2026-08-19)

This is an auxiliary negative/security regression run against the dirty current
working tree. It is retained because the source-integrity check intentionally
failed; the failure must not be hidden by updating a frozen baseline hash.

At capture time the checkout had 168 tracked dirty files and 3,988 untracked
entries. This is why the source-seal tool cannot legitimately create a new
candidate from the current tree yet.

## Command and result

```bash
PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:Experiments:pythonWrapper \
  python3 -m pytest -q \
    tests/python/test_ndnsf_di_request_cancellation.py \
    tests/python/test_ndnsf_di_runtime_aware_campaign.py \
    tests/python/test_ndnsf_di_scheduling_contract.py \
    tests/python/test_ndnsf_di_execution_consistency.py \
    tests/python/test_authorization_evaluation.py \
    tests/python/test_spec164_artifact_manifest.py \
    tests/python/test_ndnsf_di_tuning_cache_contract.py \
    tests/python/test_ndnsf_di_engine_contract.py
```

```text
77 passed, 1 failed in 4.45 s
```

The sole failure was
`test_baseline_source_hash_inventory_matches_current_subject`. The test
detected that the current working tree is not the source subject captured by
`specs/172-data-centric-authorization-evaluation/contracts/runtime-gates.json`.
The mismatch is a source-integrity/freeze failure, not a test flake.

## Mismatched frozen-baseline entries

The complete read-only comparison found these eleven entries out of the
registered inventory. Seven are dirty in the current checkout; four already
differ at `HEAD` because the Spec172 inventory predates the current branch:

```text
examples/App_Provider.cpp
examples/App_User.cpp
ndn-service-framework/ServiceController.cpp
ndn-service-framework/ServiceProvider.cpp
ndn-service-framework/ServiceProvider.hpp
ndn-service-framework/ServiceUser.cpp
ndn-service-framework/ServiceUser.hpp
ndn-service-framework/common.hpp
ndn-service-framework/utils.cpp
tests/unit-tests/encrypted-permission-response.t.cpp
tests/unit-tests/generic-dynamic-api-crypto-auth.t.cpp
```

At least `examples/App_Provider.cpp` contains an intentional large-data helper
change, while `App_User.cpp`, `ServiceController.cpp`, `common.hpp`, and the
two listed crypto test files already differ from the older baseline at `HEAD`.
The remaining ServiceProvider/ServiceUser/utils differences are current dirty
changes. Updating one hash would therefore be insufficient. Do not rewrite
`runtime-gates.json` merely to turn this run green. First create a deliberate
source candidate/seal, review the complete diff, and regenerate the baseline
inventory as part of that candidate.

## Interpretation

The cancellation, runtime-aware scheduling, authorization, artifact-manifest,
tuning, and engine tests that ran before the inventory check passed. The
negative/security axis remains **PARTIAL** until the source identity is sealed
and the registered inventory is regenerated and rerun. This result reinforces
the exact-source SIF and T029 freeze blockers; it does not indicate that the
NDNSF protocol implementation itself failed these 77 cases.

## Focused named-negative rerun

A second, narrower run of the named lifecycle/authorization negative files was
performed after the broader run. It produced the same deterministic source
identity failure and no additional protocol assertion failure:

```text
57 passed, 1 failed in 3.22 s
failure: test_baseline_source_hash_inventory_matches_current_subject
first reported drift: examples/App_Provider.cpp
```

The focused result confirms that the remaining failure is the frozen-baseline
gate, not an intermittent negative-case failure. It does not replace the
broader 77/78 result above and does not authorize changing the registered
inventory without first selecting and sealing the intended source candidate.

## Registered selector and candidate-integrity checks

The executable selector check was rerun separately so the baseline-drift
failure cannot hide whether the registered negative gates themselves run:

```text
23 passed in 2.21 s
```

This included every registered Boost.Test selector, runtime-boundary mapping,
evidence-bundle freeze/mutation check, group-capability check, assignment-v3
check, admission lifecycle, execution-lease integration, and request-
cancellation contract. Thus the selector registry is executable; the current
source remains unqualified only because its eleven hashes do not match the
older frozen subject.
