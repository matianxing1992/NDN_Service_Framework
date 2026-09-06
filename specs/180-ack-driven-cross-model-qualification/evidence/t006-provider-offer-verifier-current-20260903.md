# Spec180 T006 provider-offer verifier evidence

**Status**: partial implementation; live qualification remains blocked.

`ProviderOfferTrustVerifier` now loads the candidate-bound
`spec180-provider-offer-trust-v1` policy, requires registered Ed25519 public
keys and the existing Trust Schema verifier callback, validates ACK signer /
KeyLocator / wire-digest provenance, binds Provider/service/request/attempt,
checks model/graph/deadline freshness, and verifies the `ProviderOfferV3`
signature before the planning view is constructed. `v3_provider_view_factory`
invokes its ACK-aware method when supplied; legacy fixture callbacks remain
available only for compatibility tests.

Command:

```text
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 -m pytest -q tests/python/test_spec180_provider_offer_trust.py
7 passed
```

This is focused source evidence, not proof of a live Trust Schema certificate
chain or ACK-to-Response execution. T006 remains open until the maintained
network path supplies the real verifier and its mismatch/revocation negatives.
