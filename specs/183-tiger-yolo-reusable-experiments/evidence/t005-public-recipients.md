# T005 public recipient material checkpoint

Date: 2026-09-07. Status: PARTIAL; not runtime qualification.

The maintained User grant seam now accepts `NDNSF_DI_RECIPIENT_PUBLIC_KEY_MAP`:
Provider identity maps to a relative public PEM path and SHA-256. The existing
security owner performs bounded reads and rejects traversal, symlinks, duplicate
fields, changed bytes, private PEMs and unsupported curves. Ed25519 and P-256
public keys are supported. The map itself must be authenticated by preparation;
its embedded hashes alone do not authorize a Provider.

Spec183 User launch requires an explicit non-plaintext protection epoch, public
recipient map, and its own requester/envelope/authority key files. Provider
private files are not User inputs. The historical Spec181 private-map fixture
remains available outside this Spec183 path; configuring both maps is rejected.
The existing trusted in-process policy authority is retained, not represented
as separate-process authority isolation.

## Evidence

`python3 -m pytest -q Experiments/TigerCluster/tests tests/python/test_spec183_v3_backend_selection.py tests/python/test_spec183_public_recipients.py --tb=short --junitxml=Experiments/TigerCluster/results/spec183-public-recipients-r1/junit.xml`

Result: **311 passed in 20.04s**. This proves focused key decoding/rejection and
process-boundary argument construction, not native secure inference.

Two actual User-seam regressions were added in
`tests/python/test_spec181_y_b_grant_seam.py`: public-only recipient material
(delete the Provider private file before grant creation and verify unwrap with
the in-memory recipient key) and rejection of ambiguous maps. Running these
with `-k 'public_recipient_seam or ambiguous_maps'` produced **2 setup errors**:
the actual User import requires unavailable `ndnsf._ndnsf`. Neither test body
ran. No fake extension or skip was used. Both must pass after T008 native build.

## Remaining controlling work

### Certificate binding prerequisite for offer preparation

The maintained ProviderOfferTrustVerifier requires the actual certificate name
and key locator prefix, in addition to the distinct Ed25519 offer signer key.
`identities.issue` now derives these fields from its newly issued certificate
Data and records them in `identities.json`; it does not construct guessed key
or certificate components. `certificate_binding` validates the expected
identity and certificate-name structure using python-ndn wire parsing.

Five parsing tests passed (0.72s), including foreign identity and malformed
certificate-name negatives. Their synthetic digest-signed Data prove parsing
only, not certificate authenticity. The full focused set passed **326 tests in
21.32s**, JUnit under `results/spec183-certificate-binding-r1/junit.xml`.
The actual ndnsec issuer/installed default certificate and signed readiness
remain to be exercised inside the qualified runtime. Offer key generation and
policy construction still need wiring to these authoritative metadata fields.

### Real recipient preparation component

`runtime/identities.py::issue_yolo_recipients` now generates four independent
Ed25519 recipient key pairs and a separate User request-signing seed after
role HOME preparation. It reuses identity-name validation, HOME isolation and
leases; rejects existing outputs before generation, uses exclusive 0600 file
writes, emits one private map per Provider and one public-only map for User.
An interrupted preparation is retained and cannot overwrite/reuse its partial
keys. The component is intended to run offline inside the candidate SIF; it
has not yet been connected to the final prepare command.

Real cryptography tests load generated PEMs through the maintained private and
public loaders, compare each pair, check distinct keys/permissions and reject
reuse, missing roles and duplicate identities. PIBs in these tests are explicitly
layout fixtures, not ndnsec-issued identity evidence. **4 passed in 0.71s**;
complete focused regression **321 passed in 21.13s**, JUnit in
`results/spec183-recipient-preparation-r1/junit.xml`. Tests do not generate or
silently replace catalogue/model policy trust roots. These still require
authenticated preparation and model-package verification.

### Native launcher follow-up

The native owner `NativeProtectedGrantCredentials.cpp::credentials` was checked:
it reads `SPEC181_GRANT_AUTHORITY_PUBLIC_KEY`, locates the sibling
`trust-root-registry-v1.json`, resolves `publicKeyPath` against the parent of
that directory, and looks up its own identity in
`SPEC181_PROVIDER_RECIPIENT_KEY_MAP`. The launcher now supplies these actual
settings rather than invented CLI options.

The shared public layout is `/config/contracts/trust-root-registry-v1.json`
with `/config/contracts/authority.pub`; preparation must set registry
`publicKeyPath` to `contracts/authority.pub`. The User uses this same registry.
Each Provider's private HOME contains `recipient.pem` (0600) and
`recipient-map.json`, exactly one identity mapped to that same HOME's container
path. Launch rejects missing/oversized/duplicate/wrong-identity/foreign-path
maps, missing keys and invalid key permissions. No authority private key or
peer private HOME is mounted into a Provider. The native loader retains actual
PEM and policy validation; launcher fixtures do not prove these validations.

Follow-up focused suite (same command, JUnit under
`results/spec183-provider-recipients-r1/junit.xml`): **317 passed in 20.91s**.
This is process-boundary and rejection evidence, not native credential runtime
or numerical inference evidence. Signed preparation, full key parsing and
cross-process readiness remain mandatory before qualification.

T005 stays unchecked. Implement signed preparation producing the required
authority-registry and per-role credential layout, and genuine
permission/catalogue readiness; then T006 collector. T008 must
run the full User seam tests and native-import backend tests. New source must
be sealed into the new local SIF. T007 and remote qualification remain pending;
SSH connectivity alone grants no experiment readiness.
