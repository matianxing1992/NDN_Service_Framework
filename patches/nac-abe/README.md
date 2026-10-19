# NAC-ABE patch series for Spec 210

`series.json` pins upstream commit `5ed23e68520fb9c4ef747c51d6d18c915ff88fc4`
and the ordered patch hashes. Apply the series to an isolated clean checkout of
that commit using `git apply`, in order. Do not apply it over a working checkout
with local changes.

- `0001` preserves the existing Consumer, ParamFetcher and Producer lifecycle,
  cache-generation and segmentation fixes required by the current NDNSF caller.
- `0002` adds `Consumer::consumeWithCkAuthorization`. This optional API uses the
  existing Consumer/public parameters/DKEY, independently fetches CK segments,
  and applies caller producer authorization after normal signature validation.
  It never reads or populates the legacy CK cache or in-flight callback maps.
  A Face scheduler fences cancellation, cache invalidation and shutdown.
- `0003` adds `TestProducerBoundCk` to the dependency's existing native CMake/Waf
  unit-test glob. It tests real encryption and segmented transport, with
  ValidatorNull plus explicit cryptographic signer checking. It does not prove
  deployment trust-root/delegation qualification.
- `0004` fixes relative `common.hpp` includes in exported algorithm and crypto
  headers so an installed SDK can be included independently without resolving
  an unrelated application's header of the same name.

- `0005` encodes KP-ABE's nested attribute component before appending it to
  the CK name. Without this step ndn-cxx writes an empty component, which the
  strict consumer correctly rejects. `TestProducerBoundKpCk` checks the literal
  attribute TLVs, actual decryption and rejection of a signature-valid CK from
  another identity. The existing malformed-locator test also rejects an empty
  generic attribute component before sending any CK Interest.

Build the dependency through its own CMake project with `-DHAVE_TESTS=ON`; run
`unit-tests --run_test=TestProducerBoundCk`. Follow the repository's bounded
host-build policy. NDNSF Waf consumes the installed dependency and checks both
header and linked library for the new API.

The bounded CMake build passes. `TestProducerBoundCk` passes 8 cases / 67
assertions; existing `TestConsumerRevocation` passes 7 cases / 67 assertions.
These are dependency-native results with ValidatorNull plus explicit signer
checks. Rejecting-validator/CA trust, sanitizer qualification and installer
integration remain part of T009. This directory does not change NDN-SVS or
implicitly replace an installed library.

The four-patch SDK also passes the focused 8-case / 67-assertion suite after
rebuilding. A parallel installation at `/usr/local/ndnsf-spec210-nac-r8` is
selected explicitly for current qualification; the default SDK is unchanged.
The new and legacy focused suites also pass against that installed library:
15 cases / 134 assertions, with the selected library path checked using `ldd`.
NDNSF Waf configure passes its mandatory new-API compile/link probe against
that installation. Core compilation and runtime qualification are separate.

The stack installer now selects the patched source from the NAC entry in
`packaging/host-dependencies.lock.json`. Its `patch_series` pins this directory's
series file by SHA256; that file pins the upstream commit and every ordered
patch. The source helper materializes and patches a private archive without
editing the original checkout. An existing dirty checkout is rejected intact.

The SDK prefix is `/usr/local/ndnsf-sdk/nac-abe/<commit>-<series-sha256>`.
Installation uses the existing manifest owner at `/usr/local`, with nested
root-relative staging. It leaves the default `/usr/local/lib/libnac-abe.so`
unchanged. Skip requires the matching source/upstream/library receipt and a
compile/link/loader check of `consumeWithCkAuthorization`; version 0.1 alone is
insufficient. The maintained Waf invocation receives the same explicit SDK
prefix and controlled pkg-config selection. Configure-only mode requires this
SDK already installed and does not build or fall back to the old global NAC.

These installer edits are under source review and have not yet qualified a new
installation. The earlier parallel-SDK results above do not establish that the
new installer path passes. Current Core/runtime, rejecting-validator/CA and
sanitizer qualification remain separately tracked in Spec 210.

Current five-patch native regression passes `TestProducerBoundCk`,
`TestProducerBoundKpCk` and `TestConsumerRevocation`: 17 cases / 163 assertions.
These results use the dependency build-tree library, real encryption and
segmented transport with ValidatorNull plus explicit signer checks. They do
not establish normal certificate-chain rejection or final installed NDNSF
qualification. The maintained installer derives a separate SDK prefix from the
pinned upstream commit and the exact `series.json` digest.
