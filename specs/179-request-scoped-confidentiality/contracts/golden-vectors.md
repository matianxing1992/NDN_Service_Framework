# Spec179 canonical wire vectors

These vectors freeze the canonical TLV encodings used by the request-scoped
confidentiality contract.  The test fixture is deterministic (fixed names,
version, timestamps, key bytes, nonce, digest, and signature bytes), and the
unit test asserts the SHA-256 digest of each complete encoded block.  A digest
is used for the long binding and envelope blocks so that the test remains
readable; the exact bytes can be regenerated with
`GoldenWireVectorsAreStable` and are not dependent on RSA randomness.

## Fixed fixture

| Field | Value |
|---|---|
| service | `/ObjectDetection/YOLOv8` |
| request | `/request/42` |
| attempt | `1` |
| ControllerVersion | `(1788285600123, 7)` |
| user certificate | `/user/alice/KEY/encryption/issuer/v=1` |
| provider certificate | `/provider/p1/KEY/encryption/issuer/v=1` |
| selection wrapped keys | `01 23 45 67 89 ab` |
| envelope validity | `created=1000`, `expires=2000` |
| AEAD key id | `key-1` |
| AEAD nonce | `00 01 02 03 04 05 06 07 08 09 0a 0b` |
| AEAD ciphertext | `aa bb cc` |
| AEAD tag | sixteen bytes of `5a` |
| AEAD AAD digest | thirty-two bytes of `6b` |
| AEAD event | `event-1` |
| policy validity | `100..2000` |
| policy signature bytes | `de ad be ef` |

## Expected canonical encodings

| Encoded block | SHA-256 of complete wire |
|---|---|
| `ControllerVersion` | `551f7db75b58ae93733eec80d69d8e82e8fc87a9ebdda3f5fc67f6e4f0dba0ea` |
| `RequestSecurityBinding` | `16971d8287fd77248a1bdeb985ec83d52a819b14581f944d41453aca650c72e4` |
| `SelectionKeyEnvelope` | `3279b65bdf796ade76da0ed7531023d1a29db508f871fe74090e69b912797ce5` |
| `AeadEnvelope` | `9d5f146483394b1aa9f1eb4045659c90f9bc0506036623cff9b1f645bf7e4fdb` |
| `PolicyStatusData` | `b2d69620d8423a2bc35070253dd2173cb5ebf8af02848c312d5c33835c80973d` |

The short ControllerVersion wire is also fixed as:

```text
fdf70011fdf70108000001a05e20c17bfdf7020107
```

Any change to field order, integer width, name encoding, or omitted/added
binding field must update this contract and its review evidence; it must not
silently change the vectors.
