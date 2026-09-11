# Spec180 YOLO MiniNDN Runner Contract v1

This contract defines the real Y-A/Y-B/Y-N entrypoint. It is an execution
contract, not a second placement implementation. The runner starts the
existing NFD/NDN-SVS/security services and invokes the maintained model-first
YOLO application; it must not call the legacy deployment-first oracle and
relabel its output as Spec180 evidence.

## Registered invocation

The local inventory invokes exactly one command per case:

```text
python3 Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-A|Y-B|Y-N
```

The in-image remote jobs use the thin dispatcher
`/bundle/scripts/run_spec180_case.py`; it is a dispatch boundary, not a second
runner. It MUST verify the candidate/gate manifest and route `yolo-functional`
to this same maintained YOLO implementation (and `qwen-functional` to the
separately implemented Qwen3.6-27B ONNX entrypoint) without copying protocol logic,
accepting ambient source paths, or invoking the legacy deployment-first
oracle. The remote job still fails closed with
`SPEC180_CASE_RUNNER_NOT_READY` when a candidate SIF does not contain this
executable dispatcher; source presence in the repository alone does not
qualify an image.

The dispatcher input is closed and candidate-bound. Its only model-specific
input is the candidate-digest-bound, digest-checked `SPEC180_WORKLOAD` document
mounted by `run-functional.sh`; it MUST use the dedicated
`spec180-dispatch-workload-v1` schema with exactly the following non-secret
fields: `schema`, `gate`, `case`, `entrypoint`, `args`, `environment`, and
`evidenceSchema`. `gate` MUST equal the CLI `--gate`, `case` MUST be `Y-B` for
`yolo-functional` or `QWEN-F` for `qwen-functional`, and `evidenceSchema` MUST
be `spec180-result-v1`. `entrypoint` MUST resolve beneath `/bundle` to
the maintained YOLO entrypoint or the separately implemented Qwen3.6-27B
entrypoint. `args` is an
allowlisted array of fixed argument strings; `environment` is an allowlisted
map containing only candidate-bound `SPEC180_YOLO_*` or frozen Qwen workload
variables. Unknown fields, relative/traversal paths, shell fragments, private
material, or a value not covered by the workload digest MUST fail before the
entrypoint is started. The dispatcher MUST set the child working directory to
`/bundle`, pass only this validated environment plus the fixed runtime
variables, and `exec` the maintained entrypoint so that child exit, signal,
and evidence ownership remain visible to the outer supervisor.

The registered argument vectors are fixed, not caller-selected:

```text
yolo-functional: --case Y-B
qwen-functional: --case QWEN-F
```

This vector is reserved for the separately implemented Qwen3.6-27B ONNX
runner. It is intentionally not the Spec175 M11 wrapper: routing QWEN-F to
that tiny local fixture would create a false Tiger qualification. The registered
entrypoint validates the production manifest/object set and must fail closed
with an external-input error before any workload process starts when those
inputs are absent. A QWEN-F workload may
carry only the four allow-listed non-path `SPEC180_QWEN_*` metadata fields,
including `sha256:`-formatted model-identity and prompt digests;
the fixed `SPEC180_MODEL_MANIFEST` and `SPEC180_MODEL_ROOT` runtime bindings
are the only model-manifest/root paths. The fixed runtime variables supplied
by `run-functional.sh` cannot be overridden by the workload environment map.

The dispatcher MUST NOT derive Provider names, role assignments, candidate
selection, ACK coverage, or model-layer boundaries from the workload document.
Those values remain owned by the live ACK snapshot, sealed plan, and the
maintained application/Provider path. A workload manifest can select a case
entrypoint and bind its inputs; it cannot replace the request-time protocol.
The T013 release validator MUST parse this schema and verify its file digest
before submission; the dispatcher repeats the digest/schema checks inside the
image so a changed or mismatched mounted file fails before process startup.

`SPEC180_CASE_OUTPUT_DIR` is supplied by the supervised local gate and is the
only writable evidence root. The runner must reject a missing, non-directory,
or pre-populated case output directory before starting MiniNDN.

Candidate-bound inputs are supplied through an explicit gate environment (or
an equivalent immutable run manifest):

```text
SPEC180_YOLO_CANONICAL_PACKAGE
SPEC180_YOLO_CATALOGUE_REGISTRY
SPEC180_YOLO_CATALOG_DATA_NAME
SPEC180_YOLO_CATALOG_SIGNER
SPEC180_YOLO_OFFER_TRUST_ROOT
SPEC180_YOLO_OFFER_PUBLIC_KEY_MAP
SPEC180_YOLO_OFFER_PRIVATE_KEY_MAP
SPEC180_YOLO_TOPOLOGY
SPEC180_YOLO_CONFIG
SPEC180_YOLO_NATIVE_REQUESTER_CONFIG
```

All non-secret paths must be absolute, readable, and bound to the candidate
manifest. `SPEC180_YOLO_OFFER_PRIVATE_KEY_MAP` is a protected runtime input:
it maps each declared Provider identity to an absolute readable Ed25519 private
key file, is consumed only inside the Provider child, and its key bytes and
paths are never copied into `case-input.json`, logs, or result evidence. The
descriptor may record the map/file content digests so a later process-boundary
recheck can detect replacement. Missing, changed, or ambient values fail
before NFD, Provider, repository, or user processes are started. Per-run
identities, certificates, repository
state, and temporary deployment metadata may be generated under the fresh
case directory; generated values are evidence inputs, not selection
authority.

`SPEC180_YOLO_NATIVE_REQUESTER_CONFIG` is the candidate-bound native requester
configuration consumed by the maintained User entrypoint. A Y-B workload that
omits it must fail before the User starts; the caller must not silently select
the legacy ACK-driven Python planner. The native configuration is a path input,
not a placement map, and it must be sealed and digest-checked with the other
workload inputs.

Before process startup, the runner writes a non-secret `case-input.json`
descriptor containing the package manifest, catalogue registry, topology,
configuration, trust-root, public-key-map and every public-key-file digest,
private-key-map and every private-key-file content digest (never paths or
bytes), and the canonical model relative path/digest, plus the exact catalogue
Data name, signer identity, case, and expected
candidate IDs. It also records the fixed Provider cardinality and whether the
startup capability-cover witness passed; these fields describe preflight only
and are never interpreted as a role assignment. The Data name and signer must
both be absolute NDN names. The
trust-root must use
`spec180-provider-offer-trust-v1`. A package manifest is rejected if a
secret-bearing field or value is nested anywhere in its metadata. Graph and
initializer paths must remain beneath the canonical package root; absolute or
traversal paths fail closed. This descriptor binds preflight inputs but is not
a substitute for live protocol evidence.

The catalogue Data name must also satisfy the native exact-name APP contract:
it must be below `/<catalogue-signer>/NDNSF/DI/`, and the configured signer
identity must equal the namespace prefix before `/NDNSF/DI/`. For example,
`/example/controller/NDNSF/DI/catalogue/v1` is signed by
`/example/controller`. The runner rejects a name outside that prefix or a
signer/name mismatch before MiniNDN startup; the live publisher must use the
same controller identity and exact name.

### Two catalogue records and the publication barrier

The runner carries two different signed records; they must not be conflated:

1. `spec180-yolo-catalogue-v1` is the immutable package catalogue. It binds
   certified candidate IDs, graph/semantics digests, role contracts,
   dependencies, safe cuts, ingress/egress, priority, and signature. It is an
   input to validation and ACK-driven planning, but it is not the runtime
   object catalogue and normally has no `artifactDataNames`.
2. `ndnsf-di-presplit-catalog-snapshot-v1` is the active runtime APP Data
   envelope resolved after ACK closure. It contains one ACTIVE snapshot for
   each registered candidate, including exact role/rank `artifactDataNames`,
   model/graph/semantics/candidate digests, backend, precision, and snapshot
   digest. Every named object must already be published or independently
   verified as available and candidate-bound.

The T011 publication barrier must complete artifact publication/verification
for both Y-A and Y-B candidates and publish the signed active snapshot before
starting User. `mark_catalogue_published` records the receipt for this runtime
envelope; it does not turn a package catalogue, legacy `/Stage` manifest,
offline snapshot file, or ambient path into publication evidence. The User may
resolve the active snapshot after ACK closure, but it must never receive a
Selection containing synthetic or unresolvable artifact names. A missing
candidate snapshot, missing role/rank name, digest mismatch, or failed APP
publication is a fail-closed pre-request error.

The publication owner is the controller-side orchestration in T011, not
`APPController.run()` readiness and not a caller-supplied snapshot file. After
Controller, repository, and Provider readiness, T011 must use the real
repo-backed pre-split publication path (or one extracted shared helper) to
publish every candidate-bound role/rank object, then call the controller
`ServiceUser.publish_signed_app_data` path for the active snapshot and perform
exact-name readback with the expected controller signer certificate. The
`CatalogSnapshotArtifactPublisher` resolver is not a publication substitute;
it is resolve-only for this runner boundary. User startup is forbidden until
both object availability and signed snapshot readback have passed.

The Controller treats the publication JSON as an untrusted process-boundary
input. Before signing it must validate the registered case, signer-scoped
catalogue name, package-manifest and catalogue payload digests, unique artifact
names under the controller artifact namespace, bounded metadata payload size,
and each artifact payload digest. A valid catalogue digest does not authorize a
mutated artifact list.

The runner also writes a non-secret `case-plan.json` after package validation.
It contains the package digest, the case's signed candidate IDs, each
candidate's role set, ingress/egress identities, merge kind, and signed
selection priority. For Y-N it additionally records the fixed
`Y-N-O`/`Y-N-C`/`Y-N-P`/`Y-N-R`/`Y-N-I`/`Y-N-E`/`Y-N-L` expected outcomes. The
descriptor MUST contain no Provider names, role assignments, caller-selected
subset, key material, or plaintext payload. Its authority fields are
`candidateAuthority=ACK_SNAPSHOT_ONLY` and
`providerAssignment=RUNTIME_ACK_PROJECTION_REQUIRED`; it is an execution
preflight description, not a placement decision.

The candidate config must contain `runtime.nodes.controller`,
`runtime.nodes.user`, `runtime.nodes.repo`, and
`runtime.nodes.providers`, where the latter maps each authorized Provider
identity to its MiniNDN node. The service policy for Y-A must declare only
`FullModel`; Y-B must declare the four shared-candidate roles; Y-N must declare
the union used by its fixed control/negative matrix. Each registered role must
be advertised by at least one authorized Provider. A Provider may advertise
multiple roles and a role may have multiple authorized Providers when the
fixed profile's distinct-capability-cover check passes; these are
capability/authorization entries, not assignments. The selected Provider for
each complete role must still come only from the authenticated post-ACK
snapshot and sealed plan, with one Provider per selected role. These are
startup-input checks only; they do not select a Provider.

The runner also enforces the fixed capability-profile cardinalities used by
the cases: Y-A has exactly one Provider identity and that identity advertises
only `FullModel`; Y-B has exactly four identities with a distinct capability
cover for all four shared roles; and Y-N has exactly four identities with both
that shared-role cover and at least one `FullModel` capability. The cover is a
bipartite capability witness only. It does not become a role map, does not
appear as placement evidence, and cannot replace the authenticated ACK
snapshot. This prevents a nominal four-role policy with one usable Provider
from being reported as a multi-Provider test. Y-N reuses these identities but
creates a fresh state and evidence subdirectory for every fixed subcase.

The same `runtime` object must also contain an explicit `identities` map with
`controller`, `user`, `repo`, `group`, `providerPrefix`, and
`repoServicePrefix` names plus `identities.providers`. The provider map must
have exactly the same keys as `runtime.nodes.providers`, and each value must
equal its key. These names are startup/routing inputs, not placement results;
requiring them prevents the adapter from silently falling back to the legacy
`APP_ROOT`/`AI_LAB_*` namespace. The binding is revalidated immediately
before any network is created, including that every declared node appears in
the supplied topology and that the isolated case-policy digest matches the
descriptor.

The runner also applies the maintained `ndnsf_distributed_inference.policy`
parser and its user-authorization, Provider-role-coverage, and known
runtime-compatibility checks to both the source configuration and the exact
isolated case policy. This is a read-only preflight; it does not generate the
controller policy or install certificates. A loader-incompatible policy must
fail before `Minindn.start()` or any child process.

The runtime adapter owns cleanup for the case it starts. The driver must retain
the `MiniNdnCaseRuntime` instance and call its idempotent `stop()` method from
`finally` on every path, including readiness timeout, publication failure,
ACK/Selection failure, negative-subcase completion, and successful terminal
Response. `stop()` may stop only children returned by that instance's phase
launches, then the case network; a cleanup failure is an unqualified case with
preserved evidence and must not be converted into a pass.

For compatibility with the maintained APP policy loader, the top-level
`controller` and `group` fields must equal `runtime.identities.controller` and
`runtime.identities.group`; `runtime.user_identity` and
`runtime.provider_prefix` must equal the corresponding identity entries.
These duplicate fields are checked for equality rather than inferred.

`SPEC180_YOLO_OFFER_TRUST_ROOT` is the candidate-bound NDNSF Trust Schema
anchor/policy used to validate Provider certificates and signed
`ProviderOfferV3` ACK payloads. It is not a caller HMAC key map and cannot be
replaced by a Provider-supplied key, profile override, or ambient environment.
`SPEC180_YOLO_OFFER_PUBLIC_KEY_MAP` is the candidate-bound map from each
registered signer key ID to its PEM public-key file. It is only the key
material needed by the policy verifier; certificate-chain and packet
authentication still come from the existing native Trust Schema path.
The production verifier must bind the certificate identity to the advertised
Provider and service, verify the canonical offer digest, request/attempt,
model/graph, validity window, and boot epoch, and reject unknown, expired,
revoked, or mismatched identities before an offer enters planning. The
injected verifier used by focused Python fixtures remains test-only.

The runner must not manufacture this provenance from `provider_name` or from
`SPEC180_YOLO_OFFER_TRUST_ROOT`. It must consume the signer identity/key-
locator reference and wire digest projected from the Trust-Schema-validated ACK
Data by the real ServiceUser/pybind path. If that projection is absent or the
identity does not match the ACK name and offer Provider, the case fails before
candidate feasibility and no Provider process is started.

## Reuse of the existing MiniNDN harness

The Spec180 runner must reuse the maintained MiniNDN/NFD/SVS setup rather than
create a second network harness. Direct import is preferred. If an import is
not safe, extract the helper once into a shared module and update both callers;
do not copy helper bodies into a second runner. The reusable helpers are those
already used by `Experiments/NDNSF_DI_Yolo2x2_Minindn.py`, including
`Minindn`/`AppManager`/`Nfd` startup, `NdnRoutingHelper` route installation,
`initialize_di_keychains()`, and the existing process start/stop supervision.
The runner supplies its candidate-bound topology, isolated output directory,
case policy, and role-specific commands to those helpers. It must not invoke
the legacy script's deployment-first `main()`, reuse its preplanned role map,
or copy a legacy output into a Spec180 result. This reuse rule keeps NFD, SVS,
security, route, and cleanup behavior identical while allowing the Spec180
runner to own ACK closure, plan sealing, lifecycle evidence, and case oracles.

The adapter around those helpers must be an explicit case-runtime function,
not an implicit mutation of legacy module globals. It receives the validated
`caseRuntime.nodes` map, case policy path, output root, and provider identity
list; derives route origins and process commands from those values; and returns
the started-process handles plus their log paths. Hard-coded `AI_LAB_*` node
names, the legacy `/Stage` policy path, or a legacy `provider_role_assignments`
result are invalid inputs even when they happen to describe the same topology.
The command vector is represented as `CaseProcessSpec` values first and must be
inspectable without side effects. Each spec carries one startup phase and the
runner MUST execute the phases in this exact order:

```text
start_processes(phase="control")   -> wait_for_ready(control)
start_processes(phase="providers") -> wait_for_ready(providers)
publish signed catalogue APP Data
start_processes(phase="user")      -> supervise User to terminal result
```

The control phase starts Controller then repository; the providers phase starts
all authorized capability Providers; the user phase starts the maintained
ACK-driven User only after catalogue publication succeeds. `start_processes()`
is the only adapter operation that may create children. It MUST reject an
unknown or repeated phase, reject a phase whose predecessor has not completed,
resolve every phase node before creating its first child, and retain each
child handle/log path for readiness, exit-status, and bounded cleanup checks.
The driver must call `mark_catalogue_published(data_name, signer, digest)`
with the verified publication receipt before the `user` phase; the adapter
rejects a missing, duplicate, name-mismatched, signer-mismatched, or malformed
receipt. A readiness marker is not a sleep substitute: a child that exits
before its marker or a missing marker at the deadline fails closed.
Provider commands advertise only policy-declared roles; they do not receive a
selected role map. User commands must use the registered 1500 ms ACK timeout
and the candidate-bound package, catalogue, and offer-trust inputs.

After the Controller, repository, and authorized Provider readiness markers are
observed, the runner orchestration (not a caller-provided snapshot file and not
the legacy controller deployment manifest) MUST publish the exact signed
catalogue APP Data through `ServiceUser.publish_signed_app_data`. The maintained
User child launched by the runner MUST publish the encrypted YOLO `REPO_REF`
through `APPClient.publish_application_input_reference()` before
`REQUEST_SENT`. A failure or name/signer/digest mismatch in either publication
stops the case before `REQUEST_SENT`; the publication receipts and digests
become non-secret pre-request evidence. The active catalogue payload MUST use
the resolver's `ndnsf-di-presplit-catalog-snapshot-v1` envelope and bind its
exact `recordName`/`snapshotDigest`.
If the existing helpers cannot consume these arguments safely, extract them
once into a shared harness module and make both runners call that module. The
adapter must be unit-tested with a four-Provider Y-B descriptor before any
MiniNDN process is created, including a negative proving that a node-map or
identity mismatch stops before `Minindn.start()`.

Y-B's four Provider processes may run on one MiniNDN host/node; that does not
make them one Provider. The runner must record four distinct authenticated
Provider identities and the sealed role map. The three model roles use
distinct GPUs only in the Tiger profile; the CPU Merge process remains
explicit postprocessing.

### Tiger Y-B submission boundary

The maintained Tiger Y-B profile launches one Slurm task on one allocated node.
Its renderer must pass `--native-requester-config` and must verify the supplied
topology/configuration before any child starts. If the case configuration
declares more than one distinct `runtime.nodes` host, the renderer must fail
closed with `SPEC180_TIGER_RENDER_MULTI_NODE_RUNTIME_UNSUPPORTED`; the
single-node render/dispatch result is not cross-machine evidence. A true
multi-node Tiger run requires the Spec110 allocation-topology launcher, explicit
NFD TCP/UDP faces, and the later T016/T017 no-Python gates. The native User
branch currently does not emit the legacy lifecycle/numerical files consumed by
the existing Tiger collector, so native requester wiring alone cannot close
Tiger qualification.

## Case topology

- **Y-A** starts one Provider that advertises only the atomic `FullModel` role.
- **Y-B** starts four distinct Provider identities, one for each complete role
  in the shared candidate: `BackboneNeck`, `DetectShard0`, `DetectShard1`, and
  `Merge`. The merge Provider performs only the declared YOLO postprocessing.
- **Y-N** runs one fixed matrix in fresh subcases under the same command and
  output root. `Y-N-O` reverses catalogue order and must produce the same
  candidate/role decision as the control. `Y-N-C` removes the `FullModel`
  capability and at least one required shared-candidate role capability, so
  neither registered candidate remains feasible. `Y-N-P` tampers with ACK
  signer/provenance or offer binding, and `Y-N-R` alters a
  component role kind, `Y-N-I` invokes input fetch from a non-ingress role,
  `Y-N-E` uses a stale/revoked protection epoch, and `Y-N-L` checks plaintext
  redaction. The runner chooses the smallest topology for each subcase, but
  the subcase IDs, order, expected boundary, and expected outcome are fixed by
  this contract. A negative passes only when its expected fail-closed result is
  observed before the listed boundary and no terminal success is produced.
  The aggregate Y-N marker is emitted only after all seven subcases have
  cleanly terminated and their outcomes agree. An expected negative outcome is
  recorded as a passing subcase; it does not suppress the aggregate Y-N marker.

### Negative evidence acceptance (2026-09-05 repair)

A live negative marker must bind `requestId`, `attemptId`, and `observedPhase`
to the launched User and the current ordered lifecycle journal. A label for
the intended boundary is insufficient. C/P/R/L must originate from the User's
exact production rejection. I must originate from the native Provider's real
input-access guard and additionally bind `provider`, `planDigest`, and
`errorCode=DI_INPUT_FETCH_ROLE_MISMATCH`. A generic failed Response is not I
evidence. E requires a real stale/revoked grant verification; a test-owned
exception or missing grant factory is UNQUALIFIED.

The runner must inspect every child before accepting a marker, reject unrelated
exits and forced-kill cleanup, and write `negative-evidence.json` with the
validated fields, lifecycle digest and observed child exit statuses before
writing a passing subcase result. Unrelated failures must never be relabeled
as the negative test's expected reason.

The checked-in `examples/python/NDNSF-DistributedInference/yolo_2x2/yolo_policy.yaml`
contains historical `/Stage/...` deployment-first roles. It is retained for
the explicit offline oracle and repository examples only. It must not be used
as the Y-A/Y-B/Y-N deployment policy. The runner must generate an isolated
candidate policy (or equivalent runtime role registration) whose role set and
dependency graph come from the signed canonical catalogue and selected plan;
hand-editing the shared legacy YAML is not an acceptable substitute.

## Required protocol and result evidence

Before the case marker is emitted, the runner must record and validate this
order for the same request:

```text
INPUT_REFERENCE_PUBLISHED < REQUEST_SENT < ACK_CLOSED < GRAPH_READY
< PLACEMENT_DECISION < ARTIFACTS_READY < PLAN_SEALED
< SELECTION_COMMITTED < PROVIDER_EXECUTION_STARTED < TERMINAL_RESPONSE
```

The input is published through
`APPClient.publish_application_input_reference` and fetched/decrypted only by
the candidate ingress role. The terminal result is published only by the
candidate egress role. Dependency artifacts are canonical, digest checked, and
authorized for the current protection epoch. If the canonical ONNX graph uses
external initializers, the runner must publish/bind the graph object and the
separately named initializer object, including each object's byte length and
raw digest; Provider assembly must fetch both before ONNX Runtime load. The
exporter's external-data filename is normalized to the private staging name
`model.onnx.data` before load; packing filenames are not part of canonical
identity. The recipe's normalized initializer digest is checked after the pair
is loaded. The signed root publisher must carry the same initializer name, size,
and raw digest; a runner-side file or manifest entry that is not present in the
signed root is not sufficient.
The numerical result is compared with the package full-model oracle using
`atol=1e-3` and `rtol=1e-4`.

Each required lifecycle milestone is emitted exactly once as a JSONL event
under the fresh case directory. Events carry `caseId`, request identity, and a
coordinator-bound attempt identity (both immutable for that case execution),
monotonic sequence number, timestamp, and only non-secret digests or counts.
The live driver MUST pass the request ID returned/used by the generic
coordinator and the corresponding ACK attempt identity to the journal before
the first milestone. The journal requires protocol binding by default; a
locally generated provisional ID is allowed only when an isolated journal test
explicitly opts out of binding. An unbound production journal MUST fail before
it can emit a qualification event. This prevents an evidence writer from
inventing a request/attempt lineage that is absent from the NDN exchange.
The event schema has a closed milestone-specific field allowlist: fields are
limited to declared IDs, digests, counts, statuses, and boundary labels, and
all values are recursively checked as non-secret scalar data. Generic
`payload`, `content`, `token`, `bytes`, `plaintext`, credential, or arbitrary
map fields are forbidden even if their values do not match a secret-name
pattern. The stored event file itself is an oracle and MUST have a path, schema,
and SHA-256 digest recorded in the terminal result manifest.
The current allowlist is: `referenceDigest` for
`INPUT_REFERENCE_PUBLISHED`; `requestDigest` for `REQUEST_SENT`;
`ackSnapshotDigest`/`ackCount` for `ACK_CLOSED`;
`graphDigest`/`catalogueDigest` for `GRAPH_READY`;
`candidateId`/`candidateDigest`/`candidatePriority`/`providerCount` for
`PLACEMENT_DECISION`; `artifactDigest`/`artifactCount` for
`ARTIFACTS_READY`; `planDigest` for `PLAN_SEALED`;
`selectionDigest`/`selectedRoleCount` for `SELECTION_COMMITTED`;
`roleDigest`/`providerCount` for `PROVIDER_EXECUTION_STARTED`; and
`resultDigest`/`requestCount`/`status` for `TERMINAL_RESPONSE`.
`INPUT_REFERENCE_PUBLISHED`, `REQUEST_SENT`, `ACK_CLOSED`, `GRAPH_READY`,
`PLACEMENT_DECISION`,
`ARTIFACTS_READY`, `PLAN_SEALED`, `SELECTION_COMMITTED`,
`PROVIDER_EXECUTION_STARTED`, and `TERMINAL_RESPONSE` must be strictly ordered;
the provider-execution event is emitted once for the first selected role.
Missing,
duplicate, out-of-order, or plaintext-bearing events fail the case before the
marker. Y-N subcases use the same schema and record an expected failure boundary
instead of a terminal success.

The case marker is emitted exactly once and only after all child processes have
clean exit status, no unexpected signals or survivors, redacted logs, and the
registered result oracle. A zero exit without the marker is unqualified.

## Trust boundary

The existing caller-provided HMAC offer-key map is a focused-test scaffold. It
cannot be used for local qualification, SIF replay, or Tiger evidence. Formal
Y cases require the production NDNSF Trust Schema/Provider-identity verifier
anchored by `SPEC180_YOLO_OFFER_TRUST_ROOT`; until that verifier and the runner
are implemented and convergence-audited, T011/T014/T015 remain open.
