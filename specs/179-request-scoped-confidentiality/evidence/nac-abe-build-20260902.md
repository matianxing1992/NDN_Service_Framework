# Spec179 NAC-ABE cache-invalidation build

Date: 2026-09-02  
Branch: `UAV-Experimental`  
Purpose: verify that Controller-version invalidation cannot retain stale
NAC-ABE DKEY/CK material or link NDNSF against an older installed library.

## Dependency input

- NAC-ABE base checkout: `/home/tianxing/NDN/NAC-ABE`, base commit `1cc17d9`
- Isolated worktree: `/tmp/nac-abe-spec179`
- Patch: `nac-abe-consumer-cache-reset.patch`
- Patch SHA-256: `518bd565772b3efe368f90ae9fcd79199a72bbcd6b6200f17cae3080e889ddfb`
- Installed prefix: `/tmp/nac-abe-spec179-prefix`
- `libnac-abe.so` SHA-256: `16ca4405dc5e643c924b8b0d1bb55eed4144e71eacdbbaba2978e86117950d1f`
- patched `consumer.hpp` SHA-256: `20a2c22c07b12a69a2aa809c243c065eb20370b32487aa9aa96b49a67514189d`

The patch adds `Consumer::clearCache()` with a generation fence for pending
DKEY/data/CK fetches, zeroizes cached private/encrypted key material, reports
invalidated pending decryptions, and uses the existing
`CacheProducer::clearCache()` API. The original NAC-ABE checkout was not
modified.

## NDNSF build

The Waf option `--nac-abe-prefix` was added so one dependency prefix supplies
both headers and libraries. It prepends the selected include/library paths and
adds an `$ORIGIN`-independent RPATH for the selected library. Build command:

```bash
export PKG_CONFIG_PATH=/tmp/nac-abe-spec179-prefix/lib/pkgconfig:/usr/local/lib/pkgconfig
./waf --out=build-clang-spec179-nac3 configure \
  --with-tests --nac-abe-prefix=/tmp/nac-abe-spec179-prefix
./waf --out=build-clang-spec179-nac3 build --targets=integration-tests -j1
```

The resulting framework library SHA-256 is
`f902db8d6ec572ff01662dd698acd8bba7825d1fa81a0d91fa13196e400e7ee1`.
`ldd` resolves `libnac-abe.so` to the isolated prefix and reports no
`not found` entries; the selected library exports
`ndn::nacabe::Consumer::clearCache()` and
`ndn::nacabe::CacheProducer::clearCache()`.

## Executed evidence

```bash
export LD_LIBRARY_PATH=/tmp/nac-abe-spec179-prefix/lib:/home/tianxing/NDN/ndn-service-framework/build-clang-spec179-nac3
./build-clang-spec179-nac3/integration-tests \
  --run_test='ControllerRevocationFlow,ControllerVersionRefresh,RequestScopedSelection,RequestScopedResponseConfidentiality' \
  --log_level=test_suite --report_level=short
```

Result: **41/41 test cases and 840/840 assertions passed**. This covers the
framework-side cache-clear call sites, service isolation, restart fail-closed
behavior, selected-Provider request confidentiality, tamper rejection, and
large-response trust/binding checks. It is component evidence; the
cross-process old-epoch decrypt denial, source-independent status retrieval,
and full MiniNDN matrix remain open T008/T009/T011 gates.
