# T014 — NAC-ABE upstreaming package (split-PR description)

> Historical draft, superseded by [verified delivery](nac-abe-delivery-20260905.md).
> It omits T019/T020, incorrectly calls the personal fork master upstream,
> and mixes API changes into PR-1. Do not use its split/application instructions.

Date: 2026-09-05 (CDT).  Purpose: hand-off material for promoting the local
Spec179 dependency patches into the upstream NAC-ABE repository.  Everything
here is already verified and frozen in the paired evidence:

- Local patch commit: NAC-ABE `Experimental` branch `b1c9c4f`
  (unpushed; parent is upstream `master` `1cc17d9`).
- Compile-contract proof that upstream `master` alone cannot build the
  current NDNSF framework: `nac-abe-unpatched-contract-20260905.md`
  (RV-U22, executed 2026-09-05, clean-prefix rebuild RC=1).
- Behavioral proof of the freshness defect on `master`:
  `nac-abe-dkey-freshness-20260903.md` /
  `nac-abe-consumer-cache-reset.patch` /
  `nac-abe-grant-only-dkey-refresh.patch`.

## Split recommendation: two PRs, one optional third

Splitting keeps the single-line semantic fix reviewable independently of the
API-contract extension, which touches NDNSF call sites and needs a versioned
surface.

### PR-1 — DKEY publication freshness and exact-parameter fetch (bug fix)

Pure behavioral fix, no API change.  One-line core change plus the
exact-name plumbing it requires:

| Change | Why (NDN semantics) |
|---|---|
| `AttributeAuthority::generateDecryptionKeySegments`: publish DKEY segments with `FreshnessPeriod = 0` (was `4_s`; upstream comment already marks the value as "shouldn't affect much") | A grant-only policy replacement republishes a *different* complete DKEY under the same unversioned `MustBeFresh` discovery name; a Content Store copy still fresh within 4 s answers the discovery with the pre-grant DKEY, which lacks the newly granted attribute and cannot decrypt post-grant ciphertext. `0_s` makes the discovery unsatisfiable from CS. |
| `AttributeAuthority::onPublicParamsRequest`: do not append type/version to an already-versioned exact request | `CanBePrefix=false` exact-name fetch would never match a Data name the handler produced with a duplicate appended version. |
| `ParamFetcher` binds an expected immutable public-parameter Data name and content digest, supports cache clearing, and fetches exact names with `MustBeFresh=false` | Makes the fetcher's cache lifecycle part of the fix so a stale CS/param copy cannot satisfy a versioned request. |

Evidence of the defect and the patch diff:
`nac-abe-dkey-freshness-20260903.md`, `nac-abe-grant-only-dkey-refresh.patch`
(header carries the full behavior description).

### PR-2 — cache-invalidation / DKEY-refresh API contract extension

Adds the members NDNSF Spec179 calls at compile time.  Each symbol maps to
its Spec179 use (`nac-abe-unpatched-contract-20260905.md` verified every row
against the unpatched build):

| New/patched API member | NDNSF Spec179 call sites | Spec179 use |
|---|---|---|
| `Consumer::clearCache()` (generation-scoped cache invalidation; `m_cacheGeneration` fence, invalidation callbacks) | ServiceProvider.cpp:11993/11997, ServiceUser.cpp:3992-4035 | FR-015/FR-020 — Controller-version-change ABE cache invalidation; retained identities must stop using ciphertext keys of a withdrawn generation exactly at the new status version |
| `Consumer::refreshDecryptionKey()` (DKEY-only refresh fence: at most one in-flight DKEY Interest, pending coalescing, atomic replacement only after full decode, stale-generation completion ignored) | ServiceProvider.cpp:12010, ServiceUser.cpp (same surface) | FR-017/FR-034 — grant-only target-policy replacement issues exactly one lazy DKEY fetch/install and must not tear down a still-usable old key before the replacement decodes |
| `Consumer::getPublicParamsDataName()` / `getPublicParamsDigest()` | ServiceProvider.cpp:11914/11916 | FR-038 — verify the status-carried ABE public-params name and digest bound to the accepted `PolicyStatusData` |
| `CacheProducer::refreshPublicParameters(expectedName, expectedDigest)` | ServiceProvider.cpp:12035, ServiceUser.cpp (same surface) | FR-015 — exact versioned public-params refetch after a Controller-version change |
| `ParamFetcher` expected-name/digest binding (shared with PR-1; land the contract part here if kept) | (via Consumer/Producer above) | Same FR-038 verification path |

This is the surface whose absence fails the NDNSF build on upstream master
(RV-U22); the patches in `nac-abe-consumer-cache-reset.patch` and
`nac-abe-grant-only-dkey-refresh.patch` are the current application form.
The Consumer fence is also what makes post-clear re-arming safe under the
scheduled refresh path (`PolicyRefreshCoordinator`), so the patch is not
optional: it is the enforcement half of R179-H0A's "rotates the global ABE
generation on withdrawal" claim.

### PR-3 (optional, engineering) — `OpenAbeExecutor` dedicated worker thread

Moves OpenABE calls off the caller's thread (one worker).  Independent of the
Spec179 contract; can be upstreamed on its own schedule or folded into PR-2.

## Rebuild and re-gate steps after an upstream merge

1. Merge the upstream commit (both PRs) on NAC-ABE `master`.
2. Fresh CMake build + install to a clean prefix (same method as
   `nac-abe-unpatched-contract-20260905.md`).
3. NDNSF rebuild with the exact Spec179 configure line, only
   `--nac-abe-prefix=<new prefix>` differing (e.g.
   `--out=build-clang-spec179-upstream-<sha>`).
4. Re-run RV-U20/RV-U21 (`ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy` + the mixed-generation decrypt matrix) and the
   `ControllerRevocationFlow` suite on that build; record as the T014
   acceptance evidence in `validation-matrix.md` RV-U22's row.

## External gate

Push/PR against the upstream NAC-ABE repository require the NDNSF
maintainer's explicit go (never automatic).  Until the upstream merge, all
Spec179 builds use the isolated patched prefix; `tasks.md` T014 remains open
with this package as its local-work evidence.
