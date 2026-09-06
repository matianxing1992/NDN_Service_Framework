# NAC-ABE DKEY refresh regression evidence

Date: 2026-09-03 01:28 CDT

## Root cause

Installing a newer Controller status invalidated the NAC-ABE Consumer cache.
The patched `Consumer::clearCache()` deliberately removes the identity private
DKEY as well as encrypted service keys.  The NDNSF User and Provider status
handlers cleared that cache but did not schedule a replacement DKEY fetch, so
the next protected request failed closed with `Private decryption key doesn't
exist`. This affected the runtime after an otherwise valid status update; it
was not a key-name mismatch or a MiniNDN transport failure. A grant-only
status has the opposite requirement: it must not clear an unaffected
same-generation DKEY, but it must trigger one replacement fetch for the newly
authorized identity.

## Fix

`ServiceUser::invalidateControllerScopedCaches()` and
`ServiceProvider::invalidateControllerScopedCaches()` now call
`obtainDecryptionKey()` immediately after a global `clearCache()`. For a
grant-only change, `applyPermissionResponse()` marks a pending target-only
refresh and the first accepted same-generation PolicyStatus initiates one
coalesced DKEY fetch without clearing unrelated identities. The patched
NAC-ABE Consumer keeps at most one DKEY fetch in flight, uses a DKEY-only
generation fence for grant-only refreshes, ignores stale callbacks, and installs
only a fully decoded replacement. A redacted
`NDNSF_NAC_DKEY_REFRESH_REQUESTED` diagnostic records the reason.

## Build and run

- Build directory: `build-clang-spec179-nac3`
- NAC-ABE prefix: `/tmp/nac-abe-spec179-exact-prefix`
- Compiler: `/usr/bin/g++` (GCC 9), Boost 1.71
- Build command: `./waf build -o build-clang-spec179-nac3 --targets=ndn-service-framework -j1`
- Example targets: `App_ServiceController,App_Provider,App_User`
- MiniNDN command:
  `sudo -n env PYTHONUNBUFFERED=1 NDNSF_MININDN_LOG='*=DEBUG' NDNSF_HYBRID_CRYPTO_TIMING=1 python3 tests/minindn/run_request_scoped_confidentiality.py --execute --scenario user-identity-revocation --lifetime-ms 15000 --build-dir build-clang-spec179-nac3 --output /tmp/spec179-user-identity-rerun-20260903-1`

## Observed outcome

The launcher completed with `gatePassed=true`, `networkEvidence=true`,
`executionCount=16`, `selectionPublicationCount=76`, and
`responsePublicationCount=31`.  All five processes returned zero; the
controller status source was the Controller and the terminal owner was
`/example/hello/provider/A` with reason `response_callback`.

Both providers and both users logged one DKEY refresh request for the accepted
epoch.  Provider-A subsequently logged selection, input fetch, handler
execution, and request-scoped response encryption for multiple requests; User-A
logged request-scoped input publication and response decryption.  No provider
or user log contained `Private decryption key doesn't exist` after the refresh.

This closes the previously observed DKEY-refresh regression for this real
cross-process scenario. It does not by itself close the remaining T008/T009
cross-service, restart, Targeted, large-response, or streaming requirements.
The grant-only path has source and compile evidence, but still needs a
cross-process count proving exactly one target fetch, zero unaffected fetches,
and denial of the newly granted attribute before replacement installation.

## Updated exact-prefix build

After adding the grant-only DKEY fence and DKEY `FreshnessPeriod=0`, NAC-ABE
was rebuilt and installed from `/tmp/nac-abe-spec179-exact-build` into
`/tmp/nac-abe-spec179-exact-prefix`. The NDNSF target was then linked with
`./waf build --out=build-spec179-grant-refresh --debug
--targets=ndn-service-framework -j1` (Waf success, retry completed in 8m11.429s
after one transient assembler segfault). `ldd -r` reports no unresolved symbols;
the shared library RPATH uses the exact NAC-ABE prefix, its NEEDED entries
include `libnac-abe.so` and `libndnsd.so.0.1.0`, and all resolved Boost libraries
are 1.71.0. The resulting `libndn-service-framework.so` SHA-256 is
`b7c1e2153ae2dfcc4bc6d52b50975e709ca621558c676984fa7b1635f2efa34b`. This is
a build/linkage check, not a substitute for the still-pending cross-process
grant-only cryptographic count.
