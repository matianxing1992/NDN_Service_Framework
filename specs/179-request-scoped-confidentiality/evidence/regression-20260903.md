# Spec179 regression evidence — Context Mode and LocalMock refresh boundary

Date: 2026-09-03 (CDT)

## Code fixes

- `ServiceUser` and `ServiceProvider` now mark only their `LocalMockTag`
  constructors as local fixtures.  Production constructors keep the immediate
  NAC-ABE DKEY re-arm after an accepted Controller-version change.
- LocalMock status installation remains fail-closed until its explicit fixture
  bootstrap/pump completes; it no longer starts a duplicate in-process DKEY
  fetch against the fixture Controller/AA.
- The Context Mode Codex app-server probe now sends the required
  `clientInfo.title` field for Codex 0.151's initialize request.

## Build and tests

Builds used the existing Clang/Boost 1.71 configuration:

```text
./waf build -o build-clang-spec179-nac3 --targets=integration-tests -j1
./waf build -o build-clang-spec179-nac3 --targets=unit-tests -j2
```

Both builds completed successfully.  The following integration suites passed
with the rebuilt binary:

```text
RequestScopedSelection
RequestScopedResponseConfidentiality
ControllerVersionRefresh
ControllerRevocationFlow
Spec175InvocationStream
```

The following unit suites passed with the rebuilt binary:

```text
Spec175InvocationStreamLifecycle (15)
Spec175InvocationStreamMessage (16)
RequestScopedConfidentiality (13)
ControllerRevocationPolicy (31)
ControllerRevocationState (11)
GenericDynamicApi/CryptoAndAuthorization
GenericDynamicApi/TargetedInvocation (27)
```

The previously failing live Controller refresh case now reports:

```text
NDNSF_REVOCATION_LIVE_REFRESH permission_fetch=accepted \
user_revoke=renewal_denied provider_revoke=execution_denied
```

## Context Mode/plugin checks

- `context-mode doctor` passes for Codex storage, hooks, SQLite, and MCP
  server initialization.
- `codex mcp list` reports `codegraph` and `context-mode` enabled with
  absolute commands.
- Claude standalone Context Mode storage, hooks, scripts, and SQLite pass;
  the expected warning is that the Claude Code client is not installed and
  no `enabledPlugins` section is needed for standalone MCP mode.
- The Codex guard now reaches its intended final gate:
  `NO_REAL_SESSION_EVENTS`.  A full Codex/VS Code restart and one real prompt
  are still required before a post-restart host marker can be accepted; no
  synthetic marker was used.
- `codegraph status .` reports an up-to-date index; GSD health reports no
  errors; the ARS skill is installed.  A temporary `CTX_FIXTURE_*` hook probe
  confirmed that the installed Codex UserPromptSubmit hook accepts the current
  JSON shape and writes both `user-prompt` and derived `intent` events.  The
  absence of a real prompt event is therefore a host-dispatch observation, not
  evidence that the hook script is broken.
- `codex doctor` separately reports a corrupted `~/.codex/logs_2.sqlite` and
  stale rollout references.  The guarded recovery helper refuses to move the
  database while the active app-server owns it; recovery must happen after a
  full VS Code/Codex shutdown, followed by restart and a real marker prompt.

## Privileged MiniNDN evidence

The first privileged run was launcher setup evidence, not a protocol result:
exporting `NDN_LOG='*=WARN'` into the MiniNDN process also reached NFD, whose
logging grammar rejected it before the socket became ready.  The rerun removed
the inherited logging variables and completed on the rebuilt binary:

```text
scenario=user-identity-revocation
output=/tmp/spec179-user-identity-priv2
status=completed  gatePassed=true  networkEvidence=true
processes: controller-1=0 provider-A=0 provider-B=0 user-A=0 user-B=0
executionCount=16  revocationApplied=true  refreshAttempts=21
requestPublicationCount=156  selectionPublicationCount=76
responsePublicationCount=31  responseDecryptedCount=16
redactedTraceHash=sha256:0ec78d9057e9f32f4812b1c1cb0dfcd886960cc52fa7e76c24d0e6608d53bbc7
```

The service-scoped scenario also completed after the harness moved MiniNDN's
private work directory to a short hash-derived `/tmp/s179-*` path:

```text
scenario=service-scoped-revocation-with-unaffected-control
status=completed  networkEvidence=true  gatePassed=true
processes: controller-1=0 provider-A=0 provider-B=0 user-A=0 user-B=0
executionCount=16  revocationApplied=true  requestPublicationCount=148
selectionPublicationCount=76  responsePublicationCount=31
responseDecryptedCount=16
redactedTraceHash=sha256:c7cda82ed81b23709140d66010eb20929eb93f92d5ee53f5ab7eb945b1d61a76
```

Provider-identity, Controller-restart, and offline-rejoin probes also produced
real network traces with zero process failures, but remain `gatePassed=false`:
the current role runner does not yet emit/verify provider restart or offline
rejoin transitions, and therefore these runs are diagnostic rather than
release evidence.  The first service-scoped attempts failed only because the
long output directory made NFD's Unix socket name exceed `sun_path`; the
short-workdir fix is covered by a Python regression test.  Large-response,
Targeted, and stream scenarios still require execution before T011 can close.

## Evidence boundary

These results close the rebuilt local unit/component regression and the
LocalMock live Controller-refresh failure.  They do not close T009–T013 or
the remaining T008 cross-process/source-independent cache requirements:
privileged MiniNDN network evidence, restart persistence, Targeted/large/
stream cross-process revocation, and final release audit remain open.

## Full integration-suite boundary

The complete `integration-tests` executable ran 123 cases and returned `201`
with two failures.  They are retained as separate, non-Spec179 blockers rather
than hidden:

1. `NdnsfDataV1SvsFlow/ProductionProviderContextUsesSvsSegments` times out
   because the installed SVS API's `subscribeToProducer` delivers subsequent
   publications only; it has no historical catch-up overload, while this older
   test publishes before subscribing.
2. `Spec175InvocationStream/NormalStreamCancellationFencesLaterCallbacks`
   observed two events in the full-suite schedule but passed when rerun alone;
   the isolated run is not promoted to a full-suite pass.

All Spec179-focused integration suites listed above pass independently, and
the complete unit target passes the rebuilt focused suites.  The two failures
must be repaired or explicitly quarantined before claiming a green full
integration release gate.

## Exact4 rebuild and full integration rerun

After adding the explicit NAC-ABE prefix to the Waf configuration, the
candidate was configured against the same local NAC-ABE prefix and Boost 1.71:

```text
./waf configure --out=build-spec179-exact4 --debug --with-tests \
  --enable-shared --disable-static --toolchain-root=/usr/bin \
  --nac-abe-prefix=/tmp/nac-abe-spec179-exact-prefix
./waf build --out=build-spec179-exact4 --debug --targets=integration-tests -j1
```

The integration target linked successfully; `ldd -r` returned status 0 with
Boost 1.71, the exact NAC-ABE prefix, `/usr/local/lib/libndn-svs.so.0.1.0`,
and `/usr/local/lib/libndn-cxx.so.0.9.0`.  The complete executable then ran:

```text
./build-spec179-exact4/integration-tests --log_level=test_suite
Running 124 test cases...
*** No errors detected
```

The SVS segment fixture now starts its live fetch before publication and pumps
the subscription into place; this removes the previous publish-before-
subscribe false timeout in `ProductionProviderContextUsesSvsSegments`.
The exact4 full unit target is being rebuilt separately after fixing test-only
aggregate initializers and the duplicate `HAVE_TESTS` Waf define; its result
is not claimed here until the executable completes.
