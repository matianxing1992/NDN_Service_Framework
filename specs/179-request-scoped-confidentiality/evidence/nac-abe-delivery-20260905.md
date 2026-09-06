# T014 — verified NAC-ABE delivery

Updated2026-09-05 CDT. Local package PASS; user explicitly requests no PR.
Publication is not scheduled; upstream acceptance remains open. This supersedes the earlier
split description, which omitted T019/T020 and mixed API changes into a purported
no-API-change fix. No NAC history or runtime source was changed in this step.

## Exact delivery and destination

NAC repository: `/home/tianxing/NDN/NAC-ABE`, branch `Experimental`.
Base: `1cc17d9d21f4dfc0921cc77315d0c57d46291880`.
Head: `85547eb558c4a4f706b51cb354ab4db573609895`.
Tree: `31af0e7f131b87da0ca42d80d82dd99ba4b7eed3`.
Range:18 files,1242 insertions/131 deletions.

| Commit, oldest first | Included work |
|---|---|
| `b1c9c4f076f1c49fce8225f7737348692ede01c7` | DKEY freshness, authority policy/generation API, exact parameter binding, Consumer invalidation/renewal, Producer refresh, serialized OpenABE execution |
| `8b462d09073b97bec1a7e145cefc898df0682c67` | Fence late content/CK callbacks and normalize decryption errors |
| `b3b43c8cbcd623d318508aede851e822b8b29481` | Bind warm cache to caller key, detach callback batches, fence parameter generations, prevent authority relabeling, preserve no-argument fetch overload, regression/CTest fixes |
| `85547eb558c4a4f706b51cb354ab4db573609895` | Require clean matching dependent objects for ABI migration |

Read-only live `git ls-remote` results during this update:

| Remote | Branch | Advertised revision |
|---|---|---|
| `origin`: `matianxing1992/NAC-ABE`, personal fork | `master` | `1cc17d9d21f4dfc0921cc77315d0c57d46291880` |
| `origin` | `Experimental` | Absent |
| `suravi`: `suraviregmi/NAC-ABE`, another fork | `master` | `5ac3eb991d6ed7eef36e6a265e97912961e9807f` |
| `suravi` | `ck-duplicate-interest` | `b69dc2e4aa2ce146605728c3a19f7e32e65e4431` |

The tested base is the personal fork's master. GitHub parent/source metadata
confirms the official project is `UCLA-IRL/NAC-ABE`, master
`58f394862cd2a2462fbcf763c000c745f9f7f0c8`. Both configured remotes are forks.
The previous claim about12 original-project prerequisite commits compared an
outdated fork. Against official master, fork master is2 ahead/3 behind and
Experimental is6 ahead/3 behind. See the corrected
[official comparison](nac-abe-official-comparison-20260905.md).

Per the latest user instruction, do not submit a PR. The next engineering step
is isolated qualification of the two missing official fixes, retaining local
repairs. No publication is scheduled; fork publication alone would not close FR-041.

## Review sections

These are logical sections, not independently tested branches. The complete
four-commit range is the validated delivery; a physical split needs revalidation.

### 1. Fix stale DKEY discovery after policy replacement

Only `AttributeAuthority::generateDecryptionKeySegments` freshness changes from
4 seconds to0, with its rationale comment. A grant-only update replaces the
identity's complete DKEY while retaining the unversioned discovery namespace.
Zero freshness prevents a previously cached reply satisfying fresh discovery;
exact segment retrieval remains possible. Fresh discovery depends on reaching
the authority. Historical ciphertext is not retroactively revoked.

Exact parameter naming, ParamFetcher state and new APIs belong to section2.
Historical reproduction: [DKEY freshness evidence](nac-abe-dkey-freshness-20260903.md).

### 2. Bind refresh and asynchronous work to the accepted generation

Add authority policy/generation operations and status-bound parameter refresh.
Keep grants within the same ABE generation; withdrawals rotate it. Fence delayed
callbacks/retries/validation, preserve callback reentry, and bind cached decrypt
results to scheme, parameters, private key and encrypted CK. NAC owns material
and fetch lifecycle; the Controller decides User service use and Provider
service offering permissions.

| Surface | NDNSF caller at `994018ac` | Contract |
|---|---|---|
| Authority `getPublicParametersWire`, `setPublicParametersVersion`; version getter | `ServiceController.cpp:340,343,441`; getter supports authority naming, no direct NDNSF getter call | FR-017/019: canonical parameter bytes/version for signed status |
| KP authority `replacePolicy`, `removePolicy`, `rotateKeyGeneration` | `ServiceController.cpp:404,408,437,660,664,715,727` | FR-017/020/036: replace complete policy, stop issuance, rotate withdrawal generation |
| Consumer `clearCache(expectedName, expectedDigest)` | `ServiceUser.cpp:4177,4181`; `ServiceProvider.cpp:12183,12187` | FR-019/020/023: invalidate material, silently cancel obsolete consumptions |
| Consumer `refreshDecryptionKey()` | `ServiceUser.cpp:4273`; `ServiceProvider.cpp:12351` | FR-036/038: single-flight target renewal, coalesced follow-up, old key retained until complete replacement |
| Consumer `getPublicParamsDataName`, `getPublicParamsDigest` | `ServiceUser.cpp:3843,3845`; `ServiceProvider.cpp:11912,11914` | FR-019/038: compare installed parameters with signed status |
| Producer `refreshPublicParameters`, inherited by CacheProducer | `ServiceUser.cpp:4194`; `ServiceProvider.cpp:12200`; each separately clears Producer cache first | FR-019: exact name/digest refresh; refresh alone is not CK eviction |
| Producer parameter name/digest accessors | Forward ParamFetcher state, no direct NDNSF call required | Observable parameter binding |
| ParamFetcher no-argument/exact-name `fetchPublicParams`, `clearCache`, name/digest accessors | Consumer/Producer internals | FR-019/023/038: fence Data/Nack/timeout/retry/validation; validate before installing |
| Authority `onPublicParamsRequest` | Authority behind those fetches | Current canonical name only; never relabel new parameters with an unavailable old version |
| ABESupport cache/error handling | NAC encryption/decryption runtime | FR-020/023: cached success cannot authorize a different key; failures become NacAlgoError |
| Process-lifetime OpenABE worker | Patched ABESupport operations | Tested RELIC lifecycle; no independently optional-worker variant qualified |

Old standalone `.patch` files are historical partial repairs, not the current
application form. Restore the complete range or bundle below.

## Compatibility and executed validation

Ordinary source calls remain supported, including the original no-argument
ParamFetcher member. An untyped address of its overloaded name needs an explicit
target type. Public class layouts changed: this is not a drop-in ABI update.
Rebuild all dependent libraries, executables and language extensions in clean
directories against matching headers/library; inspect actual runtime resolution.
Rollback requires a complete matched deployment and cannot retain the repaired
security claim after reverting security fixes.

Cancellation is silent; applications own terminal events/timeouts. Objects
belong to the Face event thread, outlive pending work, and must not destroy an
active Consumer from its callback. The serialized OpenABE worker lives until
process exit. No hot-unload, post-fork use before exec, parallel crypto throughput
or bounded automatic cache-eviction guarantee is made. The full contract is NAC
`docs/experimental-compatibility.md`, included in the delivery.

| Previously executed matching-pair evidence | Result |
|---|---|
| Full NAC CTest executable |42/42 cases,3299 assertions; all three examples compile |
| Installed NAC focused gate |20/20 cases,89 assertions |
| Clean NDNSF native gates |183/183 unit cases,11998 assertions;72/72 integration cases,1281 assertions |
| Final Controller network cohort at clean `994018ac` |18/18 scenarios,188 assertions, both dedicated User grant gates,33 matching artifact hashes; driver/CLI exit0 |

See [compatibility review](nac-abe-compatibility-review-20260905.md) and
[Controller grant report](provider-online-grant-20260905.md) for exact commands,
failed cohorts and limits. This is configuration-bound local evidence, not
upstream acceptance or universal timing/third-party/Python/Tiger/performance
qualification. No runtime source changed in this packaging step, so those gates
were not repeated.

## Recoverable artifact

Path relative to NDNSF root:
`results/spec179-nac-compatibility-20260905/nac-abe-experimental-85547eb.bundle`.
Complete Experimental history,3577111 bytes.
SHA-256: `172539230a2ce16c9e8770d31c7c732e706f5e992a065dfbf72d386bf649209f`.

Executed: bundle verification confirms complete history; a fresh clone at
`/tmp/spec179-nac-t014-delivery-85547eb` has the exact head/tree above and clean
status; `git fsck --full --no-reflogs` passes. This verifies source/history
recovery, not an independent runtime rebuild. The bundle is ignored generated
output, not committed to NDNSF. A ref export does not include untracked keys,
runtime results or build outputs. Local storage is not an off-host backup.

Restore to a new destination from the NDNSF root:

```sh
git bundle verify results/spec179-nac-compatibility-20260905/nac-abe-experimental-85547eb.bundle
git clone --branch Experimental results/spec179-nac-compatibility-20260905/nac-abe-experimental-85547eb.bundle /path/to/new/NAC-ABE
```

## Remaining acceptance

1. Explicit publication authorization naming repository/PR target. T014 requires
   the maintainer's explicit go for push/PR. Nothing was pushed or submitted here.
2. Upstream review/merge, recording repository, target and merged commit. Resolve
   prerequisite differences and repeat affected qualification if the base changes.
3. Build/install the merged NAC revision into a new prefix using Clang10/system
   binutils at `-j2`; rebuild NDNSF in a clean separate directory. Preserve the
   previous complete pair; verify readelf/ldd and required symbols on every target.
4. Re-run NAC ordinary tests and RV-U20/RV-U21 on that dependency:
   `ServiceControllerGrantOnlyKeepsAbeGenerationAndReplacesTargetPolicy`,
   `GrantOnlyRefreshIssuesOneTargetFetchWithZeroFanOut`,
   `LiveControllerStatusRefreshRejectsRevokedRenewal` and ControllerRevocationFlow.
   Preserve both-role grant probes and the18-scenario campaign when qualifying
   the new deployment.
5. Record new revisions/hashes/results, then close T014. RV-U22's missing-contract
   compile failure stays historical; do not relabel it as an upstream green gate.

Workflow: Context Mode project/active health pass; repository tasks verify
retrieval. An initial low-entropy T014 query guard argument was corrected to the
exact feature basename. Broad CodeGraph exploration returned unrelated symbols;
exact caller strings were then verified in source. NAC has no CodeGraph index.
Spec Kit audit/prerequisites and GSD health pass; ARS not applicable. An attempted
delete/add of the same documentation path was rejected atomically by apply_patch;
this separate current report plus historical pointer preserves the earlier record.
