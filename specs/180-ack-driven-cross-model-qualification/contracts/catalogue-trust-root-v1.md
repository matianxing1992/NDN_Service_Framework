# YOLO Catalogue Trust Root Contract v1

**Revision 112 functional-profile rule**: Spec180 validates signature and trust
binding, not production PKI administration. Its release tooling may create one
fresh experiment-only signing key before candidate sealing, write the public
identity/digest into the candidate's checked-in or generated registry input,
and keep the private key in an explicit mode-0600 secret file outside Git and
outside the SIF. The public registry bytes are then frozen into the candidate
identity. Key generation after sealing or during MiniNDN/Tiger execution is
forbidden. Passing this profile makes no production trust-root claim.

This contract defines the only authority that may certify the Spec180 YOLO
candidate catalogue. Its machine-readable registration is
`contracts/trust-root-registry-v1.json`; both files are repository inputs to
T001 and neither is supplied by a runtime environment, Tiger profile, Provider
ACK, or application caller.

## Registered trust root

The checked-in registry entry MUST contain:

- `authority_id` and `key_id` as canonical non-empty identifiers;
- the public-key algorithm and signature algorithm;
- the canonical public-key bytes or a repository path whose digest is recorded;
- the public-key SHA-256 digest;
- the catalogue schema version and accepted model/graph family; and
- the exact catalogue fields covered by the signature.

The private signing key is never committed, copied into a SIF, placed in a
profile, or sent to a Provider. Export tooling receives it through an explicit
secret-file or agent interface. A missing, changed, or ambiguous key fails
export. The only permitted key creation is the revision-112 experiment setup
step before the registry and candidate are frozen; ordinary export and runtime
paths must never create or replace a trust root.

The registry must have `status: CONFIGURED` before catalogue verification can
run. A missing or unconfigured registry is an implementation-phase blocker and
no catalogue can be exported or qualified until its public-key digest is
verified.

## Covered fields

The signature covers the canonical serialization of the catalogue revision,
model/source/graph digests, every candidate ID and digest, candidate priority,
complete role/dependency/object descriptions, safe cuts, input-ingress and
result-egress roles, merge kind/schema, and the catalogue signer metadata.
JSON/member order, whitespace, and map iteration order are normalized before
signing and verification. The verifier rejects duplicate IDs, unknown fields
that are included in the signed view, missing covered fields, and digest
mismatches.

## Runtime use

Before `ACK_CLOSED`, the coordinator may verify the opaque catalogue revision
and trust-root binding as a static safety check. It MUST NOT expose candidate
records, evaluate feasibility, or bind a Provider before `ACK_CLOSED`.

After `ACK_CLOSED`, the adapter verifies this trust root and the catalogue
signature before enumerating the two registered candidates. A catalogue signed
by another key, with an unknown revision, or with an invalid/missing signature
fails before candidate enumeration and before Selection. Provider ACKs can
advertise capability, but they never become catalogue authority.

## Evidence

Every candidate and closure manifest records `authority_id`, `key_id`, public-key
digest, signature algorithm, catalogue revision, and signature digest. It also
records the trust-root file digest and source path. Secret key material and
catalogue payloads containing input/result plaintext are excluded from logs and
evidence.
