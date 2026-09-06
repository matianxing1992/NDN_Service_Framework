# Contract: Request-Scoped Confidentiality v1

## Request protected fields

```json
{
  "serviceName": "/ObjectDetection/YOLOv8",
  "requestId": "opaque-request-id",
  "attempt": 1,
  "controllerVersion": {
    "controllerGenerationTimestamp": 1788285600123,
    "controllerEpoch": 7
  },
  "discoveryDescriptor": {
    "abeCiphertext": "<NAC-ABE ciphertext>",
    "policyDigest": "sha256:<policy>"
  },
  "userEncryptionCertificate": {
    "name": "/user/alice/KEY/encryption/issuer/v=1",
    "wireDigest": "sha256:<certificate-wire-digest>"
  },
  "inputDataPublication": "<published after Provider selection; not part of discovery>"
}
```

The discovery descriptor may be readable only by identities satisfying the
service ABE policy. The Input Data is published only after selection, is
User-signed, and is never plaintext in discovery/selection packets; it cannot
be decrypted from the service DKEY.

## ACK provider advertisement

```json
{
  "status": true,
  "providerEncryptionCertificate": {
    "name": "/provider/p1/KEY/encryption/issuer/v=1",
    "wireDigest": "sha256:<certificate-wire-digest>",
    "algorithm": "RSA-OAEP-SHA256",
    "validUntil": "<timestamp>"
  },
  "controllerVersion": {
    "controllerGenerationTimestamp": 1788285600123,
    "controllerEpoch": 7
  }
}
```

## Selection key envelope

```json
{
  "serviceName": "/ObjectDetection/YOLOv8",
  "requestId": "opaque-request-id",
  "attempt": 1,
  "controllerVersion": {
    "controllerGenerationTimestamp": 1788285600123,
    "controllerEpoch": 7
  },
  "userCertificateDigest": "sha256:<user-cert>",
  "providerCertificateDigest": "sha256:<provider-cert>",
  "selectionDigest": "sha256:<canonical-selection-metadata>",
  "inputDataName": "/user/alice/NDNSF/INPUT/<request-id>",
  "envelopeAlgorithm": "RSA-OAEP-SHA256",
  "wrappedKeyEnvelope": "<RSA ciphertext containing K_input and K_response>"
}
```

## Input Data Content

After creating the request key bundle, the User publishes the exact named
Input Data referenced by `inputDataName`:

```json
{
  "algorithm": "AES-256-GCM",
  "keyId": "request-input-key-id",
  "nonce": "<unique-input nonce>",
  "aadDigest": "sha256:<request-and-input-name-bound-aad>",
  "ciphertext": "<encrypted application input>",
  "tag": "<tag>",
  "userSignature": "<signed NDN Data fields>"
}
```

The selected Provider fetches and verifies this named Data after unwrapping
the Selection envelope. The discovery Request contains no input ciphertext.

## Response protected Content

```json
{
  "serviceName": "/ObjectDetection/YOLOv8",
  "requestId": "opaque-request-id",
  "attempt": 1,
  "controllerVersion": {
    "controllerGenerationTimestamp": 1788285600123,
    "controllerEpoch": 7
  },
  "segmentOrEventId": "result/segment-0001",
  "algorithm": "AES-256-GCM",
  "keyId": "request-key-id",
  "nonce": "<unique-per-event nonce>",
  "aadDigest": "sha256:<bound-aad>",
  "ciphertext": "<encrypted result>",
  "tag": "<tag>",
  "providerSignature": "<signed NDN Data fields>"
}
```

The Response Data remains Provider-signed NDN Data. Permission Data remains
controller-signed and recipient-encrypted, but is not a response-key carrier.

For a response larger than the configured inline threshold, the Response
payload is a `LargeDataReference` with `keyScope=request`. Its versioned
`dataName` is fetched as a segmented object; each segment contains one
Provider-signed `AeadEnvelope`, with `segmentOrEventId=response/<index>` and a
unique nonce. The User accepts the reference only while the original
invocation is pending and only after the segment sequence, request binding,
ControllerVersion, nonce registry, plaintext size, and digest all validate.
Legacy references omit `keyScope`; an unknown explicit scope is rejected rather
than treated as a service-wide key reference.

## Protected-message containers

The Core message layer carries the following optional containers as opaque
canonical TLV blocks. Their inner schemas are defined by the crypto contract;
Core does not interpret keys or ciphertext:

| Message | Container | TLV type | Meaning |
|---|---|---:|---|
| Request | `RequestSecurityBinding` | `0xF810` | Non-secret request/version/certificate binding; no application plaintext |
| Selection | `SelectionKeyEnvelope` | `0xF830` | Selected-Provider RSA-OAEP envelope containing the request key bundle |
| Response | `AeadEnvelope` | `0xF820` | Request-scoped AES-GCM ciphertext metadata and authenticated content |

Each container is optional for compatibility during migration, but a request
that advertises the Spec179 capability MUST carry the corresponding binding,
Selection envelope, and Response envelope. Duplicate containers or a block of
the wrong type are rejected before dispatch. The message round-trip tests
exercise preservation and wrong-type rejection; runtime tests remain the gate
for proving that these blocks are actually produced and consumed by User and
Provider handlers.

Request, ACK, Selection, and Response signatures or AEAD AAD cover both
ControllerVersion fields. Targeted bootstrap/refill does the same. A streaming
invocation carries the pair once in its authenticated binding; each event AAD
binds the resulting ControllerVersion digest.
