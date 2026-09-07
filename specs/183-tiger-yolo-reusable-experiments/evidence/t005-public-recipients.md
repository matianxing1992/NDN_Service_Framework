# T005 public recipient material checkpoint

Date: 2026-09-07. Status: PARTIAL; not runtime qualification.

## Repo readiness checkpoint (2026-09-07)

`apps/yolo.py` now contains an internal `repo-probe` command and its host
`wait_repo_ready` consumer. It calls the maintained
`NetworkDistributedRepoClient.capability()` (ordinary FirstResponding RPC),
not a synthetic process-alive marker. It configures normal control mode,
disables Targeted fallback and adaptive admission, checks the returned Repo
identity, and closes the client and User before exclusively writing READY.
Monotonic startup retries are separate from model requests. Invalid payload,
wrong Repo or cleanup failure are not translated into readiness success.

The parent requires a prepared Worker, pins a fresh random probe ID and exact
User/Repo identities, verifies clean finite-process exit and bounded receipt,
and rejects symlink/stale/mismatching output. `run_user(package=None)` retains
the existing HOME lease, process ownership and teardown but mounts no model.
The reserved `repo-readiness` invocation has its own directory and cannot be
reused; it is not a warmup or measured inference. This does not establish
cross-node connectivity, model availability, GPU execution or inference PASS.

Verification: 18 new component cases use explicit native User/Repo doubles;
the mount/finite-process check executes a real short-lived OS child behind the
fake Apptainer boundary. Full focused suite: **389 passed in 22.60s**;
JUnit `Experiments/TigerCluster/results/t005-repo-readiness-r1/junit.xml`.
The initial run had two test-expectation errors: the timeout cases raised the
intended `TimeoutError`, but the assertion only allowed ValueError/RuntimeError.
The assertion was corrected without weakening runtime timeout behavior.

Actual native RPC, exact SIF and remote inference remain **NOT_RUN**. The final
operator still must wire Controller publication → Repo probe → model requests,
with cross-node signed freshness checks and one bounded startup lifecycle.
T005/T006/T007 remain incomplete; no remote submission is authorized by these
component results.

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

### Controller/Provider readiness consumers

`wait_controller_publication` requires a prepared Worker, waits for the
Controller's publication-complete marker, rechecks public input integrity and
reads `/output/controller/runtime-publication-receipt.json` on the host. The
marker fences the receipt write; it is not sufficient evidence by itself.
The exact catalogue signer/name/digest and artifact name/digest set are checked
through one shared pure validator in `runtime/yolo_result.py`. The existing
MiniNDN receipt wait now delegates to that same checker while retaining its
RunnerError API. Duplicate artifact rows are rejected rather than silently
collapsed by a dictionary comprehension. Host readiness never imports a
SIF-only installed source path; that import is reserved for in-container prep.

`wait_provider_ready` waits for the exact bound Provider identity and one-role
READY line. Source inspection of `DI_NativeProviderExecutable.cpp` confirmed
it follows successful `hasProviderPermissionForService` and runtime readiness.
It does NOT mean model assembly or CUDA execution: those remain post-Selection
and require execution evidence. Repo readiness and inter-node signed probes
are not covered by these functions.

Eight tests cover the real shared validator, malformed/duplicate/missing/
changed receipt rejection, a simulated marker/write fence, exact Provider
binding and no host import of a SIF-only path. **8 passed in 0.56s** before
the final host-import assertion; full final focused **371 passed in 22.30s**,
JUnit `results/spec183-publication-readiness-r2/junit.xml`. These tests do not
perform actual APP publication or authentication. `yolo_result.py` contains
real publication validation only, not a placeholder PASS collector; T006's
request/role/edge/numerical/cleanup evaluator remains unfinished. Final
operator orchestration, Repo/network readiness and all runtime gates remain
pending. Source sealing must include the maintained MiniNDN owner changes.

### Prepared Worker construction and launch boundary

`NodeRuntime.from_preparation` verifies the externally pinned receipt/public
inventory before constructing the Worker, checks mode/rank/role and run-output
directory, and retains its own plan snapshot. A bound Worker rechecks prepared
bytes before each role launch and User invocation. It does not rehash all files
in 100 ms liveness polls. The direct constructor remains a low-level lifecycle
component for focused tests, not an authorized experiment entrypoint; T007 must
require the final operator to use `from_preparation` and separate SIF/gate checks.
The final operator consumer is still pending.

Three new tests exercise real inventory verification with layout/public-file
fixtures: changed inputs prevent construction/output creation, changed bytes
prevent child launch/lease, caller plan mutation cannot rewrite the retained
snapshot, and another run's output path is rejected. **3 passed in 0.30s**;
full focused **363 passed in 20.70s**, JUnit
`results/spec183-prepared-worker-r1/junit.xml`. No actual NDN certificates,
SIF preparation or inference is validated by these fixture tests. T005 remains
partial, with final command/readiness/T006/T007 closure outstanding.

### Public preparation inventory and receipt verification

`runtime/yolo_bundle.py::preparation_inventory` enumerates the exact public
output set: issued certificates, referenced public trust files, recipient and
offer public maps/keys, policy/native-plan/service-manifest files and catalogue
publication input. It rejects unknown files/directories before traversal,
symlinks, special files, missing output, oversized content and private PEMs.
Each expected file gets a byte count and SHA-256. `prepare_in_container` records
this set in `preparation.json` only after all preparation owners return.

`verify_preparation` requires an externally pinned receipt digest and exact
run/candidate binding, then recomputes the public inventory. It returns the
PREPARED / NOT_EVALUATED receipt, not a qualification verdict. The outer
operator/worker boundary must still call it and enforce frozen/readonly mounts;
that final consumer integration remains pending. Inventory integrity does not
authenticate arbitrary certificate bytes or prove model/permission readiness.

Ten tests use explicit non-cryptographic public-file fixtures to exercise
receipt recomputation and missing/extra/private-PEM/symlink/changed/wrong-run/
wrong-candidate rejection. **10 passed in 0.36s**; full focused **360 passed
in 20.85s**, JUnit `results/spec183-public-inventory-r1/junit.xml`. After stricter
malformed registry/digest type rejection, the same ten tests passed in **0.30s**.
No real SIF preparation or inference has run; T005 and all formal gates remain
incomplete.

### Internal prepare command and input mount scope

The actual internal entry is now `python -m apps.yolo prepare --descriptor
/inputs/prepare.json --descriptor-sha256 <pinned-digest>`. It requires an exact
descriptor schema/hash and fixes template, registry, private authority and model
paths under `/inputs` and `/artifacts`; it does not accept injected executable
or mount paths. Failure exits 2 with error type and bounded source-frame
locations, excluding exception text/private material. Success is PREPARED /
NOT_EVALUATED only. This internal command does not bypass T007 or replace the
still-pending public operator `submit.py prepare` qualification boundary.

The existing `container_command` now supports an explicitly offline,
read-only `/inputs` mount only with preparation enabled, no node/network mount
and no GPU. Normal workers cannot receive this mount. The output-root guard
allows Apptainer's pre-created *empty* root HOME but rejects old contents or
other roles; requiring a completely empty `/identities` would conflict with
the common launcher's `--home` bind.

Seven command/mount/root-guard tests pass (**0.18s**); actual module `--help`
runs locally. The first full focused run passed **349 in 20.50s** before the
empty-root regression was added. Full native preparation and output inventory
binding remain unverified; do not count mock command dispatch as execution.

Final r2 including the root-HOME regression: **350 passed in 21.13s**,
JUnit `results/spec183-prepare-command-r2/junit.xml`.

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
