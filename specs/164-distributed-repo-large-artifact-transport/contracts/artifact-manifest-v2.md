# Artifact Manifest v2 Trust Contract

## Ownership

NDNSF-DistributedRepo owns this generic immutable-artifact trust composition.
NDNSF-DI may consume it but does not redefine its wire, trust, digest,
revocation, or resume semantics.

## Canonical Encodings

All integers are unsigned big-endian values. Variable-length byte/string fields
use a 32-bit length followed by exact bytes. Decoders check the declared length
and collection count against `ArtifactLimits` before allocation and reject
truncation or trailing fields.

- `ARC2`: canonical root bytes covered by the publisher signature;
- `ARS2`: signed-root envelope containing one bounded `ARC2` value and one
  bounded signature value;
- `APC2`: canonical manifest-page bytes covered by its content digest;
- `APG2`: page envelope containing one bounded `APC2` value and its declared
  page digest.

The canonical root begins with schema identifier `artifact-root-v2`; the page
schema is `artifact-manifest-page-v2`. An unsupported version is an explicit
downgrade failure. Unknown encoded trailing fields are not ignored.

## Authenticated Root

The root signature covers:

- the complete `ArtifactReference`, including logical name, full digest, size,
  format, root name, publisher identity, and policy epoch;
- packet and chunk geometry;
- the derived Data naming template;
- the manifest-tree root digest and digest algorithm;
- signature algorithm and publisher key locator;
- validity interval;
- every critical extension identifier.

The trust context supplied to the verifier is a resolved NDNSF trust-policy
decision. It binds the trusted publisher and key locator, public key, accepted
algorithms, policy epoch, evaluation time, supported critical extensions, and
revoked keys. Constructing that context from certificates/trust schema belongs
to the later NDNSF control-path integration; arbitrary network input must never
be treated as a trust context.

RSA-SHA256, ECDSA-SHA256, and Ed25519 are explicitly negotiated and verified.
HMAC is not a public provenance algorithm for this format.

## Content-Addressed Hierarchy

The root page name is derived as:

```text
<rootManifestName>/page/<digestAlgorithm>=<pageDigest>
```

Every page digest is recomputed over `APC2`. Page children are ordered,
strictly indexed, gap-free, non-overlapping, bounded, and either another page
or one chunk descriptor. Referenced pages and chunks must all exist, be
reachable exactly once, and cover the artifact exactly once.

Chunk ranges are derived from the signed `chunkBytes`; segment coordinates are
derived from signed `packetPayloadBytes`. Data names replace exactly one
`{chunk}` and one `{segment}` in the signed template and must remain under the
authenticated logical-name scope.

Chunk payloads are verified against their page-bound chunk digest. The
reconstructed artifact is independently verified against the signed full
artifact digest before activation.

An empty artifact has no pages or chunks and uses SHA-256 of the empty byte
string as both full-artifact and empty-hierarchy digest.

## Fail-Closed Conditions

Verification rejects:

- invalid signatures, untrusted publisher/key binding, invalid key material,
  revoked keys, expired validity, or mismatched policy epochs;
- unsupported algorithms, capability mismatch, format/version downgrade, or
  unknown critical extensions;
- artifact, name, page, chunk, geometry, digest, or policy substitution;
- missing, duplicate, unreachable, cyclic, oversized, too-deep, truncated, or
  extended manifest structures;
- page/chunk count or cryptographic-work budgets exceeded;
- payload truncation, extension, or digest mismatch;
- resume state with a different artifact, root digest, policy epoch, naming
  template, algorithm, or geometry.

One successful graph verification performs exactly one asymmetric root
verification. Page, chunk, and full-object digest operations remain separately
observable.
