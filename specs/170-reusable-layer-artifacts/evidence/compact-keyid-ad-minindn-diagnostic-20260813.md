# Spec 170 compact-key-id AD diagnostic

**Status**: diagnostic only; this does not close T025--T029 or authorize any
TigerCluster D gate.

## Scope and candidate

- Source fix commit: `c45c65752a5d07994aa165a692348327e9f16793`
- Runtime library SHA-256:
  `088fbec4cc1339d7b0bca228eff918c14aaa12dc52a9c6bd000cd5a518ee30d4`
- Python 3.8 extension SHA-256:
  `43d7d367e3a9658639bdf74a152b4fd0728e4b3757fa3e61073f0a24d4217af9`
- Carrier: wired MiniNDN, three independent fake Providers, V3 request-first
  selection, security enabled, admission control disabled.

## Failure and fix

Before the fix, the producer and consumer had the same scope-key digest but
different associated-data digests. The v2 envelope decoded `keyId` as its
8-byte compact hex identifier while the producer authenticated the original
`collab|request|scope` string. Stage 1 therefore rejected the Stage 0 packet
with `Collaboration data authentication failed`.

`ServiceProvider::collaborationAssociatedData()` now canonicalizes `keyId`
through `hybridCompactKeyId()` before constructing the AEAD associated data.
The wire identifier and the authenticated identifier are consequently the same.

## Verification

The repaired run is retained in:
`results/spec170-real-minindn-authfix-20260813T0840Z/`.

- Auth trace: Stage 0→1 and Stage 1→2 producer/consumer AD digests match.
- Provider logs: zero `Collaboration data authentication failed` entries.
- User: one complete three-stage response; `status=true`;
  `LLM_PIPELINE_MININDN_OK`; distributed latency `650.64 ms`.
- Formal Python gate command:
  `SPEC170_RUN_REAL_MININDN=1 PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper pytest -q tests/python/test_spec170_real_minindn_gate.py`
  Result: `3 passed, 1 skipped`.
- Spec170 Python regression command:
  `PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper pytest -q tests/python/test_spec170*.py`
  Result: `58 passed, 2 skipped`.
- Eight non-fault Spec170 C++ integration cases passed. The complete nine-case
  selection also includes a pre-existing deterministic-fault counter failure
  (`duplicatedPackets == 1`, observed `2`); that failure is retained and is
  not attributed to this AD fix.

## Boundary

This run is a control-path/fake-runtime diagnostic. It is not the required
three clean-start real-Qwen T026 campaign, exact-SIF Gate C, or any TigerCluster
D0/D1/D2a/D2b/D2h result. The remote project currently has no Spec170 SIF;
only older Spec160--Spec168 releases are present. The next gate is to finish
the Spec170 pre-freeze tasks, build and hash an exact SIF, then submit D0 first
through Slurm.
