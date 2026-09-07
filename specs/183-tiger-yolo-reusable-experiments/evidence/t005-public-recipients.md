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

### Preparation orchestration wiring

`apps/yolo.py::prepare_in_container` now connects the actual existing owners
for an audited offline invocation inside the exact candidate SIF:

1. Require initially empty `/config` and `/identities`; compare input template,
   registry and package-manifest digests against caller-supplied candidate pins.
2. Project only run authorization identities into the frozen four-role policy;
   retain model/dependency declarations and prohibit local model artifacts.
3. Invoke `build_yolo26n_adapter` for signed catalogue/graph verification and
   the maintained policy loader before identity-generation side effects.
4. Issue actual NDN identities, import pinned trust material, create isolated
   recipient/offer keys and a fresh User request-envelope key.
5. Reuse the installed MiniNDN owner's `_materialize_case_config('Y-B', ...)`
   to include the canonical Repo service permissions; reuse `write_policy_bundle`
   for native execution plan, service manifest and Trust Schema.
6. Reuse `build_runtime_publication_file` for the Controller-owned Y-B batch.
   This is metadata preparation, not host-side APP publication. Runtime User
   still publishes encrypted graph/weights and Providers fetch them over NDN.

The normal/single-node/CPU modes all use the same Y-B workload; mode changes
placement/backend, not the catalogue graph. Output `preparation.json` says
PREPARED / NOT_EVALUATED, never PASS. Public manifest binding, final five-command
entrypoint, native import qualification, actual execution and readiness remain
unclosed. Do not invoke this internal function outside the audited prepare
boundary or claim it has run merely because the code is connected.

Five new tests cover real configuration projection, unchanged source template/
dependencies, exact role capabilities and rejection of foreign identities,
missing roles and local-model bypass. **5 passed in 0.12s**; complete focused
suite **343 passed in 20.50s**, JUnit
`results/spec183-prepare-wiring-r1/junit.xml`. These do NOT execute the full
preparation function, real ndnsec issuer or native model adapter. T008/T011
must exercise the complete installed path; T007 must audit all command callers.

### Controller receipt output repair

Preparation-path review found `_publish_spec180_runtime` always wrote its
receipt beside publication input. Spec183 binds `/config` read-only, so this
would fail after successful APP publication. The maintained Controller now
accepts `--spec180-runtime-receipt-file`; Spec183 passes
`/output/runtime-publication-receipt.json`. The original default remains for
existing MiniNDN runners. Explicit output rejects input aliasing, symlinks,
traversal and existing receipt before creating the publication User, and uses
exclusive creation. It does not make `/config` writable.

A verbatim-function test with fake transport verifies real filesystem writes,
unchanged read-only input directory and no overwrite; launcher tests verify
the real argv boundary. **40 passed in 2.39s**; full focused set **338 passed
in 20.61s**, JUnit `results/spec183-controller-receipt-r1/junit.xml`. Added
overwrite/input-alias assertions passed in a focused rerun (**1 in 0.11s**).
This is not authenticated APP publication evidence. The changed installed
Controller must be included in the next source seal and local SIF. T005 remains
partial: final prepare/runtime publication/readiness and T006 are not complete.

### Pinned trust material import

`identities.install_yolo_trust` imports the existing configured registry only
when its bytes match the caller's authenticated candidate digest. It checks
the registered YOLO model family/epoch, Ed25519 algorithms and public-file
digests, then verifies the supplied 0600 policy-authority private key against
the registered public key. It preserves registry bytes and all three public
trust roots (catalogue, modelManifest, artifactPolicyAuthority). The native
`authority.pub` locator is only a public alias; no trust root is regenerated.
Only the explicitly trusted User HOME receives the authority private key.
Output directories are exclusive; failure/reuse never overwrites prior files.

Six tests use actual keys and the maintained authority registry/private-key
loaders. They verify successful import without registry rewriting and reject
registry/public hash changes, wrong private keys, unsafe private mode and
unregistered epoch before output. **6 passed in 0.65s**; full focused set
**337 passed in 21.89s**, JUnit under
`results/spec183-trust-preparation-r1/junit.xml`. After tightening JSON field
types, the same six tests passed again in **0.64s**. No catalogue signature,
model manifest, NDN permission readiness or inference acceptance is implied.

Next: final prepare must invoke these existing components inside the candidate
SIF, authenticate the source trust digest, validate the signed canonical model
through the maintained adapter, generate the remaining envelope/runtime
publication inputs and orchestrate bounded readiness. T005 remains partial;
T006 and formal gates are still pending.

### Offer preparation component

`identities.issue_yolo_offers` generates one independent Ed25519 offer key in
each Provider HOME and public keys under `/config/offers`. It generates the
existing `spec180-provider-offer-trust-v1` policy and the User's existing
key-ID-to-public-file map. Key IDs are SHA-256 of raw Ed25519 public bytes,
matching the maintained verifier. Provider/service, certificate name, key
locator, candidate ID/digest and Trust Schema identity are explicit bindings.
Certificate names are parsed from each actual certificate input, not guessed.

Existing output/private keys, invalid candidate inputs, missing/foreign
certificates are rejected before key generation. It reuses the exclusive 0600
credential writer and HOME leases. It does not authenticate certificates or
replace ACK Trust Schema validation; the final prepare owner must bind these
generated public outputs to the run manifest and validate issued identities.

Five new tests exercise actual Ed25519 sign/verify, exact public key IDs and
certificate fields, plus pre-mutation rejection. Together with recipient tests,
**9 passed in 1.15s**. Complete focused suite: **331 passed in 20.04s**, JUnit
`results/spec183-offer-preparation-r1/junit.xml`. These tests still use layout
PIBs and synthetic certificate Data, not a running SIF or authenticated ACK.
Final prepare integration, full native User verifier test, policy-authority/
catalogue material authentication, readiness and T006 remain pending.

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
with `/config/contracts/authority.pub` as the native locator alias. Preparation
preserves the original registry's `publicKeyPath` and corresponding public
file bytes; it must not rewrite catalogue/model trust roots. The User uses
this same registry. The initial instruction to rewrite `publicKeyPath` was
superseded after checking the actual native locator behavior.
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
