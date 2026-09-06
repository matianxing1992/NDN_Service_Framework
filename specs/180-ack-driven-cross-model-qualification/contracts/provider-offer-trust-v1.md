# Spec180 Provider-Offer Trust Contract v1

This contract defines the candidate-bound policy used to verify the signed
`ProviderOfferV3` object carried by an ACK. It closes the policy boundary
without creating a second discovery protocol or a caller-owned key map.

## Authority and input

`SPEC180_YOLO_OFFER_TRUST_ROOT` names one immutable, candidate-bound JSON
policy. The policy is generated or selected by the release closure and its
digest is included in the candidate manifest. It is not read from a Provider
ACK, profile override, ambient environment, or the focused-test HMAC map.

The policy delegates certificate-chain and ACK-Data authentication to the
existing NDNSF Trust Schema and Provider identity verifier. It may restrict the
set of Provider identities, services, certificate/key-locator prefixes, and
signer key IDs accepted for this candidate, but it must not replace the global
NDNSF trust owner or invent a second authorization protocol.

The canonical policy shape is:

```json
{
  "schema": "spec180-provider-offer-trust-v1",
  "candidateId": "<candidate>",
  "candidateDigest": "sha256:<64 lowercase hex>",
  "trustSchema": "<repository-registered NDNSF Trust Schema identity>",
  "entries": [
    {
      "provider": "/<provider identity>",
      "service": "/<unified service name>",
      "keyLocatorPrefix": "/<provider identity>/KEY/",
      "signerKeyId": "<registered Provider key id>",
      "certificateName": "/<provider identity>/KEY/<id>/self/<version>"
    }
  ]
}
```

Member order and whitespace are normalized before hashing. Duplicate provider /
service entries, path escapes, empty identities, unknown fields, a candidate
digest mismatch, or an entry outside the configured Trust Schema fail closed.

## Verification sequence

For every positive ACK entering V3 planning, the verifier MUST:

1. consume the signer identity, typed KeyLocator reference, and complete ACK
   wire digest projected from the Trust-Schema-validated Data packet;
2. bind that evidence to the ACK name, request ID, service, and advertised
   Provider; and
3. decode the canonical `ProviderOfferV3`, verify its Ed25519 signature over
   `offer.digest()`, and check the offer request/attempt, model/graph digest,
   validity window, boot epoch, signer key ID, service, and Provider against
   the policy and the current request deadline.

Missing or malformed provenance, an unknown/revoked certificate, a key-locator
or signer mismatch, an altered offer, a stale/expired offer, or an offer from a
Provider not listed for the service is rejected before candidate feasibility,
Selection, or Provider execution. A failed verification produces only a
structured non-secret reason.

The verifier is a pure validation component: it does not reserve resources,
close the ACK window, choose a candidate, or fetch application input. The
registered ACK timeout remains the sole discovery-closure authority.

## Evidence boundary

The policy file digest, schema version, and accepted identity summaries may be
recorded in candidate evidence. Private keys, certificates containing secret
material, plaintext input/result bytes, and the focused-test HMAC map are never
recorded. This contract is necessary for T006/T011 implementation; the file's
presence alone is not production-verifier or qualification evidence.
