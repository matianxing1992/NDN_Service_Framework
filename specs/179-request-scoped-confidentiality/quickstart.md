# Spec179 Validation Guide

Run from the repository root. The order is deliberate: cryptographic contract,
normal flow, large/streaming flow, ControllerVersion/revocation, then MiniNDN.

## 1. Contract and primitive tests

```bash
./waf build -j4
./build/unit-tests --run_test=RequestScopedConfidentiality
```

Expected: canonical AAD, RSA-OAEP envelope, AES-GCM, nonce uniqueness,
recipient binding, tamper rejection, and no plaintext/key leakage.
The Python binding is not a current implementation gate for these C++ wire
primitives; do not cite a nonexistent Python test as evidence.

## 2. Normal User/Provider flow

```bash
./build/integration-tests --run_test=RequestScopedResponseConfidentiality
```

Expected: ABE-protected discovery, request-key input, selected-Provider key
delivery, User-only response decryption, and no service-DKEY response wrapping.

For a configured User/Provider that has installed a signed, non-zero
`ControllerVersion`, an ordinary V2 request defaults to
`RequestScopedConfidentialityV1`; application input is removed from the
discovery Request and is sent only in the selected Provider's encrypted Input
Data. The temporary `NDNSF_REQUEST_SCOPED_COMPATIBILITY=1` rollback switch was
removed with the old service-wide response-key carrier once the migration
MiniNDN and streaming gates passed (T012); a stale value of the variable is
ignored, and Controller-free LocalMock/unit callers do not acquire the default
merely by constructing a request.

## 3. Large and streaming flow

```bash
./build/integration-tests --run_test=RequestScopedStreamingConfidentiality
```

Expected: one invocation response key, unique nonce per segment, segment-bound
AAD, successful ordered decryption, and nonce-reuse rejection.

## 4. ControllerVersion lifecycle, refresh, and revocation

Run the currently implemented deterministic Controller inventory first:

```bash
./build/unit-tests --run_test=ControllerRevocationPolicy
```

The current focused Controller-policy executable passes 31 pre-rekey cases, including
invalid writer/start inputs, zero-generation rejection, and pre-start
epoch-advance refusal. This evidence remains valid for version, persistence,
identity, and certificate behavior, but no longer closes revised RV-U05 or
T007: it does not distinguish `/PERMISSION/S` from `/SERVICE/S` and does not
execute RV-U20 retained-old-DKEY exclusion. Future validity, global certificate
replacement, and conflicting equal-version status remain covered.

The current state-model suite also includes the audit-added cases:

```text
HigherHintNeverBecomesAuthorityBeforeExactStatusFetch
RefreshCoordinatorHandlesTerminalAndExpiryBoundaries
```

This proves that a higher message hint cannot replace the last authenticated
status before an exact signed-status fetch. The configured-runtime no-status
branch is covered by the component case below.

Only after the deterministic suite passes, run the currently implemented
component flow:

```bash
./build/integration-tests --run_test=ControllerRevocationFlow
```

The current component flow closes the executed slice (state decisions,
durable Controller state, status serialization, policy-snapshot filtering,
immutable version-addressable recipient-bound permission-handler output, identity-wide and
certificate-only zero-grant denial, status signature negatives, and the normal
LocalMock User/Provider Request publication/execution boundary in RV-I23).
The current component flow includes the configured no-status regression:

```text
ConfiguredControllerFailsClosedBeforeStatusInstallation
```

It proves that a Controller-configured User refuses Request publication and a
Provider refuses execution before any authenticated service status is installed.
The current Controller component flow also executes the repeated timestamped
status publication, restart monotonicity, stale exact-status refusal, and
signed-status renewal-denial cases. The following behaviors remain required
before the revocation claim can be complete:

```text
production-runtime trust-schema permission renewal denial
fresh global ABE public parameters and filtered DKEY reissuance
retained old DKEY rejected against new-generation ciphertext
grant-only ControllerVersion advance with unchanged public parameters,
one lazy target-policy/DKEY refresh (complete retained-plus-new attributes),
old target DKEY denied for the new attribute until replacement install, and no
unaffected-identity refresh
exact-version hint refresh in every live message handler
Targeted token/refill, stream Provider enforcement, restart/offline, and
MiniNDN propagation
```

The repeated Controller status handler and restart case execute the
timestamp/version monotonicity and old exact-name refusal checks. The live
relay case exercises one signed status refresh and renewal-denial path, but
configured trust-schema validation, certificate-only/unaffected controls, and
exact-version hint refresh in every handler remain open.
The cross-node propagation and Targeted/large/stream request-handler suites
required for RV-I01–RV-I14 remain to be implemented by T008/T009 and must run
before any network claim.
Historical-key and already-started-execution limitations remain explicit.

The request-scoped response component currently has three focused cases:
`SelectedProviderReceivesExactEncryptedInputOnly` exercises the 9,000-byte
large-response reference path and Selection replay boundary, while
`ModifiedInlineResponseCiphertextIsRejectedBeforeDelivery` flips one byte in a
short inline response and confirms authentication fails before delivery while
the untouched response still completes. The third case installs a newer
revoking status after Provider execution and confirms that User delivery is
denied with one timeout and no application callback. These cases are component
evidence, not configured trust-schema or MiniNDN evidence. The stream component
regression separately passes 19/19, including rejection of an already-buffered
event at final delivery after User revocation.

## 5. MiniNDN security matrix

```bash
python3 tests/minindn/run_request_scoped_confidentiality.py \
  --scenario controller-restart

# Explicit network gate (requires root and the runtime-control preflight):
sudo -n python3 tests/minindn/run_request_scoped_confidentiality.py \
  --execute --scenario controller-restart \
  --output results/spec179-request-confidentiality
```

The matrix must include two Users, two Providers, User and Provider identity
revocation, `/PERMISSION/S` and `/SERVICE/S` withdrawal with unaffected and
dual-role controls, global ABE rekey plus retained-old-DKEY rejection,
grant-only `/PERMISSION/S` and `/SERVICE/S` target-policy replacement with
unchanged public parameters, one lazy target DKEY fetch/install, and no DKEY
fan-out,
in-flight revocation,
repeated Controller restarts, offline epoch skipping, newer/older/forged
ControllerVersion pairs, bounded self-refresh, Controller-unavailable expiry,
hintless scheduled revocation discovery,
large-response/Targeted/stream invalidation, stale/revoked material, tampered
Selection/Response, replay, wrong recipient, nonce reuse, source-independent
status retrieval, and controlled compatibility rollback.
Every case records a terminal stage and redacted evidence; no private key or
plaintext is retained.

The first command is contract-only and never produces network evidence. The
second command refuses to start until all deterministic Controller withdrawal
controls are available. Do not start this network gate while any RV-U or RV-I row in
`specs/179-request-scoped-confidentiality/validation-matrix.md` remains failing.
