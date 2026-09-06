# Spec179 restore session fixes — 2026-09-04

Branch: `UAV-Experimental`. Machine reboot wiped `/tmp` (NAC-ABE prefix,
worktree, build state). This session restored the canonical build and fixed
the defects that the freshly executed Spec179 suites exposed.

## Environment restore

- NAC-ABE rebuilt from the Spec179-patched working tree of
  `/home/tianxing/NDN/NAC-ABE` (commit `1cc17d9` + uncommitted Spec179
  patches including `OpenAbeExecutor`) into
  `/tmp/nac-abe-spec179-exact-prefix` with Clang 10 (GCC 9 ICEs on
  `src/ndn-crypto/data-enc-dec.cpp`).
- Canonical NDNSF build: `build-clang-spec179-nac3` (Clang, exact NAC-ABE
  prefix, `--toolchain-root=/usr`).

## Dependency fixes (NAC-ABE working tree)

1. `AttributeAuthority::onPublicParamsRequest` appended `<ABE-TYPE>/v=<n>` to
   every request, so the Spec179 exact-name public-parameter fetch
   (`CanBePrefix=false`) produced a Data name that could never satisfy the
   request. Fixed: skip the suffix when the Interest already carries
   `<ABE-TYPE>/v=<version>`. Without this, the post-status-install NAC
   producer re-arm silently never completed and the production ACK CK wrap
   returned empty content.

## Framework fixes

2. `ServiceController::grant()` now registers the granted identity's
   certificate with the Attribute Authority when the Controller keychain
   knows it (`m_aa.addNewPolicy(cert, policy)`), falling back to the
   name-only policy otherwise. Runtime-granted identities can now obtain
   their lazy DKEY without a network certificate fetch.

## Test fixture fixes

3. `ndnsf-di-core-flow.t.cpp`: restored two `[&, index]` value captures that
   the previous session had changed to `[&]` (dangling reference to a loop
   variable). The out-of-bounds write corrupted memory and caused the
   full-suite SIGSEGV in `ProductionNativeHandlersRejectTamperedD2bCapability`
   plus 11 DI failures.
4. `NdnsfIntegrationEnvironment`:
   - SVS publications are now signed with each role's real certificate
     (production shape) instead of a digest signer, so the request-scoped
     response transport-owner check can read the Data KeyLocator.
   - Added `attributeAuthorityPublicParametersName()` /
     `attributeAuthorityPublicParametersDigest()` accessors.
   - Added `pumpUntilWithAttributeAuthority()` — the normal request pump
     deliberately skips the AA face; the post-install NAC re-arm needs it.
5. `request-scoped-response-confidentiality.t.cpp`:
   - The custom request publisher now emits the production
     HybridMessageEnvelope shape and seeds the Provider receive keys
     (`cacheHybridReceiveKeyForTest`), instead of publishing the raw request
     wire the production ingress cannot decrypt.
   - The installed status now binds the fixture AA's real public-parameter
     name/digest.
   - The ambiguous "ATTRIBUTE"/"/CK/" interest probe was removed; the old
     carrier absence is asserted through the compat counters.
6. `controller-revocation-flow.t.cpp`: the RV-U20 mixed-generation decrypt
   matrix asserts fail-closed via `decryptFailsClosed` — OpenABE either
   throws or returns non-plaintext content for mismatched generations; both
   outcomes prove the plaintext is unavailable.

## Execution boundary

This evidence records code fixes and focused executions only. The full
suite baselines, MiniNDN campaign, and release audit are recorded separately.
