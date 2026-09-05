# NAC-ABE Experimental compatibility review

Status: T020 local dependency, clean rebuilt NDNSF and full MiniNDN gates PASS.

Scope: `b1c9c4f` (Spec179 dependency changes) and `8b462d0` (late callbacks and
OpenABE error normalization). Both are on local NAC-ABE `Experimental`, not
pushed. NDNSF online campaign at `7e5ef367` remains frozen historical evidence.

Pre-implementation audit: bounded repair is necessary and remains within NAC
ownership. No permission policy or wire format addition is proposed. Tests must
reproduce CK fan-out reentry, stale parameter delivery, current-generation exact
naming and unauthorized decrypt after cache warming. Preserve existing public
source calls, distinguish ABI changes from source compatibility, and retain
silent invalidation cancellation already used by NDNSF. Full dependent rebuild
is required if a public class layout changes. Test/fix/evidence form T020.

Workflow: Context Mode project/active health passed at start; CodeGraph queried
NDNSF callers, NAC has no index; Spec Kit constitution/plan/tasks reviewed; GSD
debug state in `.planning/debug/nac-compatibility-review.md`. ARS not applicable.

## Findings and changes

| ID | Severity | Confirmed issue and origin | Resolution |
|---|---|---|---|
| NC-1 | HIGH | Inherited process-global AES cache keys only by encrypted CK. Authorized warm-up lets a wrong or empty CP/KP key decrypt. This predates both reviewed commits; previous negative tests cleared the cache first. | Hash a length-delimited scheme/public-parameters/private-key/encrypted-CK tuple; preserve authorized cache reuse. `src/algo/abe-support.cpp:339`. |
| NC-2 | HIGH | CK fan-out iterates its live map. A success/error callback invoking the new invalidation API destroys the iterator; both tests crash. | Detach batch and erase old in-flight state before callbacks, then check generation between waiters. Reentrant retry remains live. `src/consumer.cpp:348,365`. |
| NC-3 | HIGH | Spec179 parameter invalidation does not fence old Data, retries or asynchronous validation. A delayed old reply overwrites newer parameters. Failed digest checking also destroys an installed snapshot. | Generation checks at every delayed boundary and candidate validation before installation. `src/param-fetcher.cpp:59,129`. |
| NC-4 | HIGH | Spec179 exact-name support reflects the request's old version while publishing current parameter bytes. | Always construct the actual current canonical name; respond only if the Interest matches it. `src/attribute-authority.cpp:173`. |
| NC-5 | MEDIUM | The earlier Spec179 changes alter public class layouts and replace a no-argument member signature; default arguments do not preserve that function's ABI. | Restore the no-argument overload; measure layouts and require a complete matching dependent rebuild. No drop-in binary compatibility claim. |
| NC-6 | LOW, tests | Default lifecycle test requires an unset role variable; CTest lacks the source fixture directory and supplies the old `-x` option. | Default normal lifecycle test to User while preserving explicit role validation; configure CTest's working directory and report option. |

The decryption boundary also bounds reads by actual ciphertext buffer lengths,
rejects missing material with `NacAlgoError`, and tests missing NUL terminators
and null content-key inputs. No ciphertext wire fields or public crypto method
signatures are added. Tests follow existing headers/namespace conventions; the
new callback regression file now has the project's license header and style.

Repair commit: NAC-ABE `b3b43c8` on `Experimental`, not pushed. Compatibility and
lifecycle documentation is `NAC-ABE/docs/experimental-compatibility.md`, linked
from its README. The change uses existing generation/callback/cache ownership;
it adds no NDNSF permission policy or general plugin framework to NAC.
Documentation follow-up `85547eb` requires a clean dependent build for ABI
upgrades; it changes no library source. These are local commits only.

## Executed evidence

Root: `results/spec179-nac-compatibility-20260905/` in the NDNSF workspace.

| Gate | Result / artifact |
|---|---|
| Warm cache negative before repair | Four failed assertions across CP/KP wrong and empty keys; `red-cache.log` |
| Old parameters and mislabeled authority before repair | Four failed assertions; `red-params-authority.log` |
| Callback reentry before repair | Two separate memory-access aborts; `red-reentry-success.log`, `red-reentry-error.log` |
| GDB controlled old-binary replay | SIGSEGV in `unordered_map::erase` from CK completion after callback invalidation; `reentry-gdb-red.log`. Old binary and prefix retained locally. |
| First complete suite | 41/42 cases,3297/3298 assertions; sole failure is unset lifecycle role, `nac-full-first-red.log` |
| Final CTest | **42/42 cases,3299/3299 assertions**, exit0,97.24s; `nac-ctest-final.log`. Includes prior CP/KP/integrated tests and benchmark correctness loops; no performance conclusion is drawn. |
| Installed-prefix execution | **20/20 cases,89/89 assertions**, exit0; `nac-installed-final.log`. Explicit `LD_LIBRARY_PATH` selects the installed library for callback/parameter/authority/crypto regressions. |
| Explicit lifecycle roles | Controller, Provider, User each pass; `lifecycle-<role>.log`. These are normal exits, not the separately controlled SIGINT mode. |
| Existing examples | All three original KP Consumer/Producer/Authority examples build; `build-final.log`. Their standalone host-NFD runner was not executed. |
| Source API | Existing ordinary callers/examples compile; original typed no-argument ParamFetcher member pointer is exercised in its Constructor case. |
| Class layouts | Three compiled probes, `layout-{pre-spec179,t019,current}.log`; see below |

The provisional `focused-green.log` used the previous test executable while
new test linking was still active. It is not final acceptance; the complete
CTest above ran after the build ended and contains all added tests.

## Compatibility and migration

Measured x86-64/Clang10/libstdc++9 object sizes in bytes:

| Class | Before Spec179 (`1cc17d9`) | After T019 (`8b462d0`) | Current (`b3b43c8`) |
|---|---:|---:|---:|
| Consumer | 1824 | 2088 | 2104 |
| Producer | 2856 | 3096 | 3112 |
| ParamFetcher | 416 | 656 | 672 |
| AttributeAuthority | 1832 | 1840 | 1840 |
| KpAttributeAuthority | 1880 | 1888 | 1888 |

Thus `8b462d0` alone is a layout-neutral repair, but neither the whole Spec179
branch nor this follow-up is a binary drop-in replacement. Rebuild framework,
applications and any language extension with matching headers and library.
Symbol comparison removes only changed private helpers and restores the public
no-argument fetch symbol; this does not override the measured layout break.

Normal public API calls remain source-compatible. An untyped address of the
overloaded parameter-fetch method must explicitly select its signature. Error
handling should use `NacAlgoError`/`std::exception`, not OpenABE enums or exact
message strings. Invalidating a consume remains silent cancellation; application
timeouts/terminal notifications stay application-owned. No promise is made that
invalid old behavior (wrong-key cached decryption or old-name relabeling) persists.

The process-lifetime OpenABE worker serializes crypto and intentionally survives
until exit. Consumer/Producer/ParamFetcher are still Face-thread objects; keep
them alive through outstanding work, do not destroy a Consumer inside its own
callback, exec before reuse after multithreaded fork, and do not hot-unload the
library. Automatic cache capacity bounds and parallel crypto throughput are not
provided. These existing limits are explicit, not claimed universal compatibility.

NAC build: `/tmp/nac-abe-spec179-exact-build`, Clang10,system Boost1.71,`-j2`.
Installed prefix: `/tmp/nac-abe-spec179-exact-prefix`; former whole prefix retained
in `prefix-t019/`. Installed library SHA256:
`925e983ba167ca158ce0fc2e8dd015fd2c9afc2971cf0ccdd5b88c66637223b0`.
`nac-hashes.log`, `nac-ldd.log`, `nac-readelf.log` retain linkage evidence; no
dependency is missing. Build/install file hashes differ after CMake's RPATH
installation step, so installed-prefix execution must be checked separately.

The installed-prefix gate above passed after a tooling incident: section export
with in-place `objcopy` rewrote ELF metadata while a diagnostic test ran. That
test was stopped(exit143), retained as `nac-installed-interrupted.log`, and not
counted. Relinking unchanged objects and reinstalling restored both original
hashes (`relink-after-inspection.log`); only then was installed execution rerun.
The exported `.text` sections compare equal (`nac-text-closure.log`). Use a
read-only ELF parser or disposable copies for future inspection.

Clean NDNSF native gates pass: 182/182 unit cases (11971 assertions) and 72/72
integration cases (1278 assertions), both exit0. The fresh Clang build completed
in21m10.471s; `clang-build-final.log`, `ndnsf-unit-final.log` and
`ndnsf-integration-final.log` retain evidence. `native-closure.log` records all
six targets resolving the matching installed NAC library with no missing
dependencies. The complete existing 16-scenario MiniNDN campaign passed below.
The previous
16/16 campaign used NAC `8b462d0`; it is historical evidence and is not relabeled
as a test of `b3b43c8`. Upstream delivery and full third-party ecosystem testing
are not established by local gates; publication remains T014.

Build recovery: GCC9 first failed inside its garbage collector; a retry linked
in6m35.140s but reused old test objects predating the new NAC layout. Invalidating
90 selected stale objects exposed another GCC crash/assembler error. Those
artifacts were not tested or accepted. A fresh Clang10/system-binutils build in
`build-clang-spec179-nac-compat` retains `-j2` and `-Werror`. Clang identified two
unused `this` captures in User/Provider status-restore error callbacks; removing
them is the only NDNSF runtime-source change in this review. Build logs retain
all attempts; the old GCC tree must not be used as a matched ABI deployment.

## Full rebuilt MiniNDN acceptance

`campaign-final/` binds clean NDNSF revision
`de1eb508219849e2f0f2017c3f23bb40f38baac4` and NAC source `b3b43c8`
(documentation HEAD `85547eb`). Every run completed with `gatePassed=true`,
`networkEvidence=true`, no launcher error and CLI exit0. The driver completion
marker is retained; its PTY session was no longer available for a separate
exit-code read. `campaign-verification.log` independently verifies all16 results,
161 scenario assertions, empty source diffs and33 identical artifact hashes
against disk before any subsequent App rebuild.

| Scenario | Result / scenario assertions |
|---|---|
| revocation-rotation-failure-retry | PASS14/14 |
| grant-after-permission-exhaustion | PASS dedicated grant gate;21/21 granted requests and60/60 control requests |
| grant-only-advance | PASS dedicated grant gate |
| inflight-revocation | PASS12/12 |
| user-identity-revocation | PASS12/12 |
| provider-identity-revocation | PASS14/14 |
| service-scoped-revocation-with-unaffected-control | PASS11/11 |
| offline-rejoin-epoch-skip | PASS8/8 |
| controller-cache-provider-status-retrieval | PASS14/14 |
| controller-unavailable-expiry | PASS7/7 |
| large-response-invalidation | PASS14/14 |
| targeted-refill-invalidation | PASS14/14 |
| stream-invalidation | PASS6/6 |
| hintless-scheduled-refresh | PASS6/6 |
| controller-restart | PASS15/15 |
| selection-response-tamper-and-replay | PASS14/14 |

The two grant runs use explicit `grantOnlyGateOk`, not an empty-checks pass.
Their target refreshes once, unaffected roles do not refresh DKEYs, and every
control/target terminal failure is retained. Planned Provider restart and
Controller outage produce role exit-2; their scenario checks verify recovery
or expiry. Other role exits are0. Raw rows/logs and manifests remain frozen.

This closes the NAC compatibility repair, not every possible authorization
case. The user clarified that Controller authorization covers both service use
by Users and service offering by Providers. These16 cases contain User online
grant and both-role revocation; Provider first-grant execution remains a
separate coverage gap, tracked as T021. No universal third-party compatibility,
Python-extension, TigerCluster or performance qualification is claimed.
