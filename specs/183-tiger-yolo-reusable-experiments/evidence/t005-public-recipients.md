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

T005 stays unchecked. Implement signed preparation with a deliberately valid
authority-registry relative-path layout, native Provider recipient-key wiring,
and genuine permission/catalogue readiness; then T006 collector. T008 must
run the full User seam tests and native-import backend tests. New source must
be sealed into the new local SIF. T007 and remote qualification remain pending;
SSH connectivity alone grants no experiment readiness.
