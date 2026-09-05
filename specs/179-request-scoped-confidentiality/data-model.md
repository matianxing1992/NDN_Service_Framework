# Data Model: Request-Scoped Confidentiality and Epoch Revocation

## RequestSecurityBinding

| Field | Type | Rule |
|---|---|---|
| serviceName | NDN Name | Canonical unified service name |
| requestId | opaque ID | Unique within User identity and retention window |
| attempt | positive integer | Binds retries/reselection; never reused with old keys |
| controllerGenerationTimestamp | uint64 Unix ms | Stable for one Controller generation; strictly newer after restart |
| controllerEpoch | non-zero uint64 | Increments for authorization changes within one generation |
| userEncryptionCertName | NDN Name | RSA encryption certificate named by User |
| userEncryptionCertDigest | SHA-256 | Digest of certificate wire encoding |
| providerEncryptionCertName | NDN Name | Advertised by selected Provider ACK |
| providerEncryptionCertDigest | SHA-256 | Digest of selected certificate wire encoding |
| selectionDigest | SHA-256 | Canonical digest of non-secret Selection metadata |
| inputDataName | NDN Name | Exact User-owned encrypted Input Data name published after key creation |

## RequestKeyBundle

| Field | Type | Rule |
|---|---|---|
| inputKey | 256-bit secret | Fresh per request/attempt; encrypts application input |
| responseKey | 256-bit secret | Fresh per request/attempt; retained by requesting User and held transiently by selected Provider while producing the response |
| keyAlgorithm | enum | Initial value `AES-256-GCM` |
| envelopeAlgorithm | enum | Initial value `RSA-OAEP-SHA256` |
| createdAt/expiresAt | time | Bounded by request deadline and certificate/epoch validity |
| consumed | boolean | Provider may consume once |

## InputDataPacket

The User publishes this exact named, User-signed Data packet after creating the
request key bundle and before sending Selection. Its Content is an
`AeadEnvelope` under `inputKey`; the packet name is carried in Selection and
bound to the request, attempt, epoch, certificate digests, and Selection
binding. Providers fetch and verify the packet before decrypting application
input.

## LargeDataReference

```text
dataName | objectType | objectId | keyScope? | plaintextSize | encrypted | digest
```

The optional `keyScope` is empty for the legacy service-wide response path and
is exactly `request` for a Spec179 request-scoped response. An unknown explicit
scope is invalid and MUST fail closed; it MUST NOT silently select the legacy
service-wide key. When `keyScope=request`, `dataName` identifies a versioned
segmented object whose concatenated Content consists of ordered, Provider-signed
`AeadEnvelope` blocks. Each block uses the invocation `K_response`, binds
`response/<segment-index>` in its AAD, and has a unique nonce. The reference
itself carries no plaintext or reusable service-wide MessageKey.

## SelectionKeyEnvelope

```text
version | envelopeAlgorithm | keyId | binding fields | wrappedInputKey |
wrappedResponseKey | creation/expiry | authenticated metadata
```

The envelope is encrypted to the selected Provider's RSA encryption
certificate. It is not encrypted to every ACK candidate and does not use a
service-wide permission DKEY.

## AeadEnvelope

```text
algorithm | keyId | nonce | aadDigest | ciphertext | tag | segmentOrEventId
```

AAD is the canonical concatenation of serviceName, requestId,
controllerGenerationTimestamp, controllerEpoch, attempt, User certificate
digest, Provider certificate digest, Selection digest, and segment/event
identity. Nonce uniqueness is scoped to one keyId.

## ControllerVersion

```text
controllerGenerationTimestamp:uint64 | controllerEpoch:uint64
```

The pair is compared lexicographically as a freshness signal. It becomes local
authority only after matching Controller-signed PolicyStatus Data validates.
The generation timestamp is stable across messages, is not a message-send time,
and is never compared with a receiver's wall clock.

## PolicyStatusData

| Field | Type | Rule |
|---|---|---|
| serviceName | NDN Name | Service-level policy scope |
| controllerGenerationTimestamp | non-zero uint64 Unix ms | Strictly increases across Controller restarts |
| controllerEpoch | non-zero uint64 | Increments for every authorization-relevant change within a generation |
| validFrom/validUntil | time | Controller-signed validity interval |
| revocations | RevocationTarget list | Typed identity-wide, certificate-only, `/PERMISSION/<service>`, or `/SERVICE/<service>` removals |
| policyDigest | SHA-256 | Digest of active ABE policy descriptor |
| abePublicParametersName | NDN Name | Exact immutable Data name identifying the active ABE generation; it may remain unchanged across grant-only ControllerVersions |
| abePublicParametersDigest | SHA-256 | Digest of canonical public-parameter wire bytes; prevents mixed-generation substitution |
| controllerSigner | certificate name | Must validate under controller trust schema |

## Authorization Change Classes

`ControllerVersion` orders every authorization-table change. The ABE
generation is a separate cryptographic identity defined by the exact
`(abePublicParametersName, abePublicParametersDigest)` pair.

| Change | ControllerVersion | ABE master/public parameters | DKEY policy/fetch behavior |
|---|---|---|---|
| Grant only | Advance | Reuse the controller-private master secret (master key) and current public-parameter generation; neither is sent to participants | Replace only the target identity's complete policy; after signed status validation, one coalesced target-only DKEY fetch returns a complete-attribute DKEY (or the target explicitly fetches it); a DKEY-only refresh fence rejects an overlapping stale fetch; issue/refetch no DKEY for any other identity |
| Reauthorization after an earlier revocation | Advance | Reuse the current post-revocation generation unless another withdrawal occurs in the same transaction | Issue one replacement current-generation DKEY only to the reauthorized identity; old pre-revocation DKEY remains unusable |
| Identity, certificate, or attribute withdrawal | Advance | Generate and atomically publish a fresh global generation | Reissue filtered current-generation DKEYs to every retained identity; issue none carrying the withdrawn authority |
| Mixed grant and withdrawal transaction | Advance once | Treat as a withdrawal and generate a fresh global generation | Reissue filtered DKEYs to every retained identity, including newly granted identities |

An existing DKEY remains acceptable across a grant-only ControllerVersion
change only when its public-parameter name/digest matches current signed status,
its certificate remains valid, and current status does not revoke its identity
or attributes. For the granted identity, the old same-generation DKEY remains
usable only for attributes it already held; ciphertext requiring the added
attribute is denied until the target fetches and atomically installs its
replacement complete DKEY. NAC-ABE generates that DKEY lazily from the
replacement policy. The DKEY's issuance version is audit metadata; it is not by
itself a reason to invalidate an otherwise current-generation unaffected DKEY.

## RevocationTarget

`authorizationAttribute` is one canonical NDN Name:

```text
/PERMISSION/<service>  # User grant for receiving ACK/Response and invoking the service
/SERVICE/<service>     # Provider grant for receiving Request/Selection and providing the service
```

| Kind | Required fields | Effect |
|---|---|---|
| `IDENTITY` | targetIdentity | Deny renewal and future authorization for that identity in every service status issued under the new ControllerVersion |
| `CERTIFICATE` | certificateDigest; optional targetIdentity consistency check | Reject only that certificate; a separately authorized replacement certificate is not implicitly revoked |
| `SERVICE_AUTHORIZATION` | targetIdentity; serviceName; authorizationAttribute=`/PERMISSION/<service>` or `/SERVICE/<service>` | Remove only that identity/service/attribute grant and preserve the other attribute and other services |

Unknown kinds, missing required fields, extra contradictory scope fields, and
duplicate targets with conflicting meanings fail status validation. An
identity-wide policy change advances the ControllerVersion and is reflected in
every affected service's immutable status Data; protected messages still carry
only the compact ControllerVersion.

`IDENTITY` intentionally affects all grants for that identity and therefore
MUST NOT carry `authorizationAttribute`. `CERTIFICATE` affects the exact
certificate digest and also MUST NOT carry an attribute.
`SERVICE_AUTHORIZATION` with a missing, mismatched, unknown, or duplicate
attribute is non-canonical and MUST fail before Controller epoch advancement or
status publication.

The authorization subject evaluated at runtime is:

```text
identity | certificateDigest | serviceName | authorizationAttribute
```

This attribute field reuses the existing NAC-ABE routing distinction instead
of inventing a second role namespace. It distinguishes a User's
`/PERMISSION/<service>` material from a Provider's `/SERVICE/<service>`
material. The current pre-migration implementation lacks this discriminator in
`RevocationTarget` and `AuthorizationSubject`; it therefore cannot be accepted
as complete service-level use/provision revocation.

## AuthorizationCacheEntry

```text
identity | serviceName | controllerGenerationTimestamp | controllerEpoch |
abePublicParametersName | abePublicParametersDigest | materialKind |
expiresAt | state | lastRefresh | invalidationReason
```

`materialKind` is one of `ABE-DKEY`, `CERTIFICATE`, `MESSAGE-KEY`,
`TARGETED-TOKEN`, or `SELECTION-BINDING`. State is `current`, `expired`,
`revoked`, `consumed`, or `tombstoned`.

## ConfidentialityAuditEvent

Audit events contain request/selection/model-independent identifiers, digest
values, ControllerVersion, recipient identity, algorithm identifiers, stage, outcome, and
failure code. They MUST NOT contain plaintext, private keys, raw CEKs, or
decryptable envelope contents.

## State Transitions

```text
DISCOVERY_AUTHORIZED
  -> ACK_CERT_ADVERTISED
  -> KEY_BUNDLE_CREATED
  -> SELECTION_ENVELOPE_SENT
  -> ENVELOPE_CONSUMED
  -> RESPONSE_ENCRYPTED
  -> RESPONSE_VERIFIED
  -> TERMINAL_CLEANUP

Any state -> REJECTED on ControllerVersion/certificate/AAD/tag/nonce/replay failure.
Any live state -> INVALIDATED on accepted ControllerVersion change or revocation.
```

```text
ACTIVE -> REFRESHING after an authenticated newer message version is observed
REFRESHING -> ACTIVE after authoritative refresh and version revalidation
ACTIVE -> REJECTED_OLDER_MESSAGE when the peer message version is older
Any non-quarantined state -> QUARANTINED only on authoritative revocation or invalid certificate status
```

`REFRESHING` permits one bounded, coalesced status fetch and does not accept the
newer message until Controller validation succeeds. `QUARANTINED` rejects new
protected protocol messages but keeps status-refresh/recovery available.

After authoritative revocation, the next affected protected transition moves
the request to one terminal rejected state at the responsible enforcement
node. The Controller owns issuance/renewal; the Provider owns User checks before
Selection/input/execution/result publication; the User owns Provider checks
before ACK selection and Response/stream delivery. If execution or request-key
disclosure already occurred, the audit state records that fact and does not
claim cancellation, rollback, or remote erasure. Reauthorization creates
new-version state rather than transitioning an old tombstone back to `current`.

Role-specific enforcement is additionally required at these boundaries:

| Grant withdrawn | Owner | First forbidden transition after accepted status |
|---|---|---|
| User `/PERMISSION/<service>` | User | Request or Selection publication |
| User `/PERMISSION/<service>` | Provider | input access or application execution for that User |
| Provider `/SERVICE/<service>` | Provider | ACK publication, Selection acceptance, execution, or result/stream publication |
| Provider `/SERVICE/<service>` | User | ACK selection or Response/stream delivery from that Provider |

If revocation arrives after execution has begun, work and side effects may
already exist. The conforming runtime suppresses later protected publication or
delivery, records the execution boundary, and terminates once. It does not
automatically reselect another Provider unless the application supplied an
idempotency/deduplication contract.
