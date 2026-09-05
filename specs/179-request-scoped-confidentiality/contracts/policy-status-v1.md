# Contract: Signed Policy Status and Revocation Data v1

```json
{
  "schema": "ndnsf-policy-status/v1",
  "serviceName": "/ObjectDetection/YOLOv8",
  "controllerGenerationTimestamp": 1788285600123,
  "controllerEpoch": 8,
  "validFrom": "<timestamp>",
  "validUntil": "<timestamp>",
  "policyDigest": "sha256:<active-policy>",
  "abePublicParametersName": "/controller/NDNSF/ABE-PARAMS/<abe-generation-id>",
  "abePublicParametersDigest": "sha256:<canonical-public-parameters-wire>",
  "revocations": [
    {
      "kind": "IDENTITY",
      "targetIdentity": "/user/revoked"
    },
    {
      "kind": "CERTIFICATE",
      "certificateDigest": "sha256:<revoked-cert-wire>"
    },
    {
      "kind": "SERVICE_AUTHORIZATION",
      "targetIdentity": "/provider/p1",
      "serviceName": "/ObjectDetection/YOLOv8",
      "authorizationAttribute": "/SERVICE/ObjectDetection/YOLOv8"
    },
    {
      "kind": "SERVICE_AUTHORIZATION",
      "targetIdentity": "/user/u1",
      "serviceName": "/ObjectDetection/YOLOv8",
      "authorizationAttribute": "/PERMISSION/ObjectDetection/YOLOv8"
    }
  ],
  "controllerCertificate": "/controller/KEY/signing/issuer/v=1",
  "signature": "<controller-signed NDN Data>"
}
```

Validation rules:

1. Controller signature and certificate validity must pass trust-schema checks.
2. `controllerGenerationTimestamp` and `controllerEpoch` must be non-zero; the
   ordered pair is the authoritative `ControllerVersion`.
   The generation timestamp is compared only as part of this ordered pair; it
   is never compared with a receiver's wall clock or treated as message time.
3. Protected V2 paths require exact equality with the accepted
   `ControllerVersion`; zero is never a wildcard.
4. A ControllerVersion update always refreshes authorization status, but it
   invalidates ABE material only when the signed public-parameter name/digest
   changes or when current status withdraws authority carried by that material.
   Grant-only changes preserve unaffected current-generation DKEYs. The target
   identity uses a DKEY-only refresh fence to reject an overlapping stale fetch,
   retains its old DKEY until a complete replacement is installed, and permits
   at most one coalesced follow-up. Request
   keys, Targeted tokens, selection bindings, nonce state, and incomplete
   requests are invalidated only when their authenticated binding becomes
   obsolete.
5. Revocation prevents future renewal and future protected transitions after
   the responsible node accepts status; it does not recall cached Data, erase
   plaintext, or remotely erase request keys already disclosed under an old
   valid version.
6. The immutable Data name is
   `/<controller>/NDNSF/POLICY-STATUS/<service>/version/<generation>/<epoch>`.
   The generation and epoch components use NDN non-negative-integer (the
   `Name::appendNumber`/`toNumber`) encoding, not UTF-8 decimal components.
   Thus a message carrying a newer pair identifies the exact status Data to
   fetch. A node may retrieve the same Data from the Controller, a Provider,
   or an NDN cache, but acceptance depends on Controller signature and full
   content.
7. `IDENTITY` requires only a target identity and is reflected in every
   affected service status under the new ControllerVersion. `CERTIFICATE`
   requires one certificate wire digest and does not revoke a separately
   authorized replacement. `SERVICE_AUTHORIZATION` requires identity, exact
   service name, and exactly one `authorizationAttribute`:
   `/PERMISSION/<service>` for a User's grant or `/SERVICE/<service>` for a
   Provider's grant. It does not affect another service or the other attribute
   held by the same identity.
8. Unknown kinds, missing required fields, contradictory extra scope fields,
   and conflicting duplicate targets fail the entire status validation; nodes
   never partially apply a malformed revocation list.
9. `authorizationAttribute` is forbidden on `IDENTITY` and `CERTIFICATE`
   targets. A missing, unknown, service-mismatched, or duplicate attribute on
   `SERVICE_AUTHORIZATION` is non-canonical and MUST be rejected before
   ControllerVersion advancement.
10. With the current NAC-ABE implementation, withdrawal advances
    ControllerVersion and creates a fresh global ABE master-secret/public-
    parameter generation. The master secret is never published. Every
    still-authorized identity obtains a newly generated DKEY containing only
    its retained attributes, and every new ciphertext uses the new public
    parameters. Consequently, a retained old DKEY can decrypt only old-
    generation ciphertext. Attribute-local rekey is not part of this v1
    contract.
11. A grant-only change advances ControllerVersion but retains the current
    controller-private ABE master secret (master key) and exact public-parameter
    name/digest. Neither secret nor public-parameter private material is sent
    to a User or Provider. The current KP-ABE
    implementation represents an identity's authority as one monolithic
    policy/DKEY, so the Controller replaces only the target identity's
    complete policy; NAC-ABE generates one complete replacement DKEY lazily on
    that identity's next DKEY fetch. After the target installs the newer signed
    status, its runtime SHOULD initiate one coalesced target-only fetch, with an
    explicit fetch as fallback. The replacement contains all retained
    attributes plus the new grant. Existing DKEYs for unaffected identities
    remain usable and are not refetched; no global DKEY fan-out is permitted.
    Until the target installs the replacement, its old same-generation DKEY is
    limited to its previous attributes and cannot satisfy the new grant. A
    reauthorized identity receives a DKEY under the current post-revocation
    generation; its pre-revocation DKEY never becomes valid again. A
    transaction containing any withdrawal follows rule 10 even if it also
    contains grants.
12. PolicyStatus binds the exact public-parameter Data name and canonical wire
    digest independently of ControllerVersion. DKEY Data identifies both its
    issuance ControllerVersion and that public-parameter name/digest. A runtime
    MUST validate the Controller signature, current status, identity and
    certificate, attribute authorization, and public-parameter digest before
    installing or retaining a DKEY. Mixed ABE generations fail closed; an older
    issuance version alone does not invalidate an unaffected DKEY when current
    signed status references the same ABE generation.

## Controller restart rule

Before publishing protected status or keys, the Controller atomically persists:

```text
newGenerationTimestamp = max(currentUnixTimeMs, previousTimestamp + 1)
controllerEpoch = 1
```

Every authorization-relevant change increments `controllerEpoch`. One
Controller identity has one active writer lease backed by the same authoritative
durable store as the generation state. Lease acquisition is atomic, returns a
fencing value, and is revalidated immediately before every protected status/key
publication. Missing/corrupt generation state, persistence failure, loss of
lease, or lease conflict stops protected issuance. Independent active-active
Controller stores are outside this contract.

## Compact message reference

Request, ACK, Selection, Response, and Targeted bootstrap/refill carry one
compact `ControllerVersion` sub-block containing only:

```text
controllerGenerationTimestamp:uint64
controllerEpoch:uint64
```

These fields are freshness signals covered by message signatures or AEAD AAD;
they are not authority. A newer authenticated pair triggers one bounded fetch
of the exact immutable Controller-signed status named by that pair. An older pair is rejected and the
receiver returns its compact current pair in the signed protocol result.
