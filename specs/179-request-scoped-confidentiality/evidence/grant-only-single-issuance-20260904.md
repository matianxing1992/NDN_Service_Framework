# Grant-only single DKEY issuance — RV-U20/RV-U21 evidence

Date: 2026-09-04 (CDT).  Binary: `build-clang-spec179-nac3`
(Clang 10; `LD_LIBRARY_PATH=/tmp/nac-abe-spec179-exact-prefix/lib`).
Worktree HEAD `e2d793e8` plus today's two grant-only refresh fixes.

## Semantic fixes under test

1. **Cross-service public-parameter identity** — the
   `abeGenerationChanged` decision compares the *consumer's installed*
   ABE public-parameter name/digest with the incoming status instead of
   treating every first install as a generation change
   ([ServiceUser.cpp:3850-3859](ndn-service-framework/ServiceUser.cpp#L3850-L3859),
   [ServiceProvider.cpp:11994-12003](ndn-service-framework/ServiceProvider.cpp#L11994-L12003)).
   A consumer that already holds parameters identical to the status is
   never classified as a generation change, so a grant-only Controller
   version advance does not wipe the NAC caches.
2. **Identity-wide refresh wave dedupe** — the consumer DKEY is
   identity-wide, so several service status installs of one permission
   wave (same `ControllerVersion`) collapse into a single replacement
   fetch: a `m_lastDkeyRefreshWave` optional records the wave already
   refreshed, and the grant-only refresh runs only when the wave
   changes ([ServiceUser.cpp:3872-3875](ndn-service-framework/ServiceUser.cpp#L3872-L3875),
   [ServiceProvider.cpp:12016-12019](ndn-service-framework/ServiceProvider.cpp#L12016-L12019);
   members [ServiceUser.hpp:1619](ndn-service-framework/ServiceUser.hpp#L1619),
   [ServiceProvider.hpp:1544](ndn-service-framework/ServiceProvider.hpp#L1544)).
   The pending marker is consumed even when the refresh is suppressed.

Before these fixes, the same scenario issued two overlapping wire
fetches (09-03 trace `/tmp/grantonly_trace.log`: `/HELLO` epoch-3 status
install at `.429504` and `/Store` epoch-3 install at `.429789`, 375 µs
apart, both requesting a grant-only refresh).  The 09-03 evidence file
[grant-only-dkey-20260903.md](grant-only-dkey-20260903.md) records that
state.

## Case execution

```text
$ NDN_LOG='nacabe.*=DEBUG:ndn_service_framework.*=INFO' \
    ./build-clang-spec179-nac3/integration-tests \
      --run_test='ControllerRevocationFlow/GrantOnlyRefreshIssuesOneTargetFetchWithZeroFanOut' \
      --log_level=message
*** No errors detected            (exit 0)
```

Case: [controller-revocation-flow.t.cpp:2903-3059](tests/integration-tests/controller-revocation-flow.t.cpp#L2903-L3059).
Wire counter attaches after the seed install
([t.cpp:3005-3025](tests/integration-tests/controller-revocation-flow.t.cpp#L3005-L3025))
and counts raw `/DKEY/...` Interests by the clean trailing identity leaf
(no full-URI match: NAC-ABE appends raw key-name TLV wire bytes after
`/DKEY`).

Assertions (all passed):

- `grantedUserDkeyFetches == 1U` after the grant-only advance
  (line 3041) — exactly one target-only DKEY fetch.
- `providerDkeyFetches == 0U` (line 3042) — unaffected identity never
  refetched.
- `grantedUserDkeyFetches == 1U` after a repeated current-wave cycle
  (line 3050) — idempotent, zero additional fetches.
- `abePublicParametersName/Digest` unchanged across the advance
  (lines 3033-3036) while `ControllerVersion` strictly advanced
  (line 3032).

## Captured wire timeline (this run, single grant-only wave)

Compact trace of `/tmp/grantonly_trace_wave.log`, times relative to the
controller start.  `NDNSF_NAC_DKEY_REFRESH_*` are the ServiceUser
refresh events; `nacabe.Consumer ... Fetch private key` + `SegmentFetcher
completed` are the wire fetches.

```text
0.060  controller start (generation=1788541272060 epoch=1)
0.904  NDNSF_CONTROLLER_GRANT user-grant-fetch /HELLO            (seed)
0.905  Consumer /example/hello/user-grant-fetch Fetch private key   (seed setup)
0.906  Consumer /example/hello/provider        Fetch private key   (seed setup)
1.707  user.fetchPermissionsFromController → epoch-2 wave
1.712  NDNSF_NAC_DKEY_REFRESH_PENDING grant-only            (pre-counter install)
1.716  NDNSF_NAC_DKEY_REFRESH_REQUESTED epoch=2 grant-only   (pre-counter)
1.716  Consumer Fetch private key (596 B, completed 1.728)   (pre-counter)
2.509  NDNSF_CONTROLLER_GRANT user-grant-fetch /NDNSF/DistributedRepo/Store   ← counted window
2.510  user.fetchPermissionsFromController → epoch-3 wave
2.512  Installed PolicyManifest policyEpoch=3; PENDING grant-only
2.513  Installed 8 user permissions (HELLO ×4, Store ×4)
2.518  NDNSF_NAC_DKEY_REFRESH_REQUESTED epoch=3 grant-only     ← exactly ONE
2.518  Installed PolicyStatus /HELLO; PolicyStatus /Store      ← second install, no refresh
2.518  Consumer Fetch private key (852 B, completed 2.534)     ← exactly ONE fetch
3.311  user.fetchPermissionsFromController (idempotent cycle) epoch=3 unchanged
3.321  Installed PolicyStatus /HELLO + /Store                  ← zero REQUESTED, zero fetch
```

Reading: within the counted grant-only wave (epoch 3), the first status
install (`/HELLO`) issues the single identity-wide replacement fetch; the
second status install (`/Store`, same `ControllerVersion`) is consumed
by the wave dedupe and does not overlap — this is the fence behavior
that previously produced the 09-03 double fetch.  The repeated
current-wave cycle at 3.311 installs statuses with no refresh at all
(idempotence).  The unaffected `provider` identity performs zero wire
fetches across the whole counted window.

## Matrix mapping

- **RV-U20** (old/new public parameters and DKEYs around one
  withdrawal; every same- and mixed-generation decrypt combination) →
  `ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy`
  ([controller-revocation-flow.t.cpp:743](tests/integration-tests/controller-revocation-flow.t.cpp#L743),
  mixed-generation decrypt matrix at
  [t.cpp:855-935](tests/integration-tests/controller-revocation-flow.t.cpp#L855-L935)):
  retained old DKEY cannot decrypt new-generation ciphertext;
  retained-identity replacement DKEY decrypts retained ciphertext;
  provider-old-key vs retained-cipher and all fresh mixed-generation
  combinations fail closed; historical old-params + old-DKEY decrypting
  old ciphertext remains the documented historical limitation
  (content-key cache is keyed per ciphertext, so reuse cannot mask a
  generation mismatch — [memory note: fresh ciphertexts required in the
  matrix]).  Suite `ControllerRevocationFlow` exit 0 on 2026-09-04.
- **RV-U21** (grant-only version advance with unchanged parameters and
  master-secret generation; one target-only DKEY fetch; fence;
  unaffected DKEYs not refetched; pre-revocation DKEY unusable) →
  `GrantOnlyRefreshIssuesOneTargetFetchWithZeroFanOut` (this file,
  single-issuance assertions + trace) plus
  `LiveControllerStatusRefreshRejectsRevokedRenewal` (revoked renewal
  rejected after withdrawal; pre-revocation DKEY unusable and the
  post-withdrawal reauthorization requires the new pair) and
  `ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy`
  (component: parameters/generation unchanged across the grant-only
  advance).  All in suite `ControllerRevocationFlow`, exit 0.  The two component
  cases were additionally re-run individually on 2026-09-04 (exit 0,
  "No errors detected") as direct execution records for this mapping.

Red/green context for the surrounding regression runs is in
[regression-red-green-20260904.md](regression-red-green-20260904.md).
