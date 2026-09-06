# Spec180 audit iteration 53 evidence

**Date**: 2026-09-03  
**Subject**: ACK packet-retention wiring and post-fix local checks  
**Verdict**: implementation checkpoint only; formal validation remains blocked

## Source correction

The production `ServiceUser` ACK regex subscription previously passed
`packets=false` to `SVSPubSub::subscribeWithRegex`. That mode delivers only the
payload and leaves `SubscriptionData::packet` empty. The ACK-aware V3 trust
boundary consequently could not obtain the authenticated Data signer,
KeyLocator, or complete wire digest. The subscription now passes `packets=true`.
The response subscription is unchanged; only the ACK path requires this
packet-backed provenance.

## Verification

```text
./waf build --targets=ndn-service-framework -j2
'build' finished successfully (2m11.103s)

native_import=PASS
ack_candidate_trust_field=True
ldd: repository build/libndn-service-framework.so.0.1.0,
      repository ndn-svs/build/libndn-svs.so.0.1.0,
      repository .local-boost171/lib/libndn-cxx.so.0.9.0 and libnac-abe.so,
      system Boost 1.71 log/chrono/thread

PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
python3 -m pytest -q tests/python/test_spec180_*.py \
  tests/python/test_spec170_default_application_path.py
114 passed, 19 warnings in 54.54s

python3 -m py_compile \
  NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/provider.py \
  NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/placement.py \
  NDNSF-DistributedInference/ndnsf_distributed_inference/app_sdk/client.py \
  NDNSF-DistributedInference/ndnsf_distributed_inference/provider.py \
  pythonWrapper/ndnsf/service.py \
  examples/python/NDNSF-DistributedInference/yolo_2x2/user.py
PASS

audit_speckit_structure.py ... --strict
PASS (25 FR, 9 SC, 20 tasks, 25 traced requirements)

scripts/spec180_contract_gate.py
PASS (contractReady=true, qualificationReady=false,
      readinessScope=DOCUMENT_AND_TRUST_ROOT_CONTRACT,
      trustRootStatus=CONFIGURED, issues=[])

git diff --check
PASS

python3 scripts/spec180_inventory.py ... --output /tmp/spec180-inventory-audit.json
FILE_MISSING:case-Y-A (expected fail-closed result; the maintained
`Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` runner is not yet present)
```

The test wall time is a diagnostic only and is not a performance result.

## Remaining blockers

The native marker now has a real packet-retention path, but it still records
that the configured Trust Schema accepted the packet; it is not an independent
certificate-chain proof. T006/T009/T011 still require a real Trust Schema
callback and ACK → Selection → Provider → Response execution in the maintained
Y-A/Y-B/Y-N runner. That runner is still absent, so T014 cannot return a
convergence `PASS`, `qualificationReady` must remain false, and no MiniNDN,
SIF, Tiger, or performance claim is authorized.
