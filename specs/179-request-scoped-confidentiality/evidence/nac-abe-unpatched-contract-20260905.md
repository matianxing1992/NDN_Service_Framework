# NAC-ABE unpatched-master contract verification

Date: 2026-09-05 (CDT).  Purpose: T014 input — decide whether the local NAC-ABE
patches (`b1c9c4f`, Experimental branch, unpushed) are *required* by the
Spec179 runtime, and characterize what upstream `master` (1cc17d9) is missing.

## Method

1. Detached worktree of upstream `master` at `1cc17d9` (no Spec179 patches).
2. Fresh CMake build (clang++-10, Debug), installed to a clean prefix.
3. NDNSF configured in an isolated out dir with
   `--nac-abe-prefix=<unpatched prefix>` (exact-Spec179 configure line,
   only the prefix differs) and compiled with
   `./waf build ... --targets=ndn-service-framework -j2`.

## Result: compile fails (RC=1)

The framework cannot compile against unpatched NAC-ABE.  Missing members
(first failing translation unit `ServiceProvider.cpp`):

| NDNSF call site | Missing member | Spec179 use |
|---|---|---|
| ServiceProvider.cpp:11914 | `Consumer::getPublicParamsDataName` | verify status-carried public-params name |
| ServiceProvider.cpp:11916 | `Consumer::getPublicParamsDigest` | verify status-carried public-params digest |
| ServiceProvider.cpp:11993 / 11997 | `Consumer::clearCache` | invalidation after Controller-version change |
| ServiceProvider.cpp:12010 | `Consumer::refreshDecryptionKey` | post-clear DKEY re-arm |
| ServiceProvider.cpp:12035 | `CacheProducer::refreshPublicParameters` | exact versioned public-params refetch |

`ServiceUser.cpp` calls the same API surface (clearCache/refreshDecryptionKey/
refreshPublicParameters at ServiceUser.cpp:3992-4035).

Conclusion: the Spec179 runtime **depends on the patched API surface at
compile time**; the patch is not optional.  Anyone combining upstream master
with the current NDNSF source fails the build — this is reproducible, not an
interpretation.

## Behavioral gap (freshness) on unpatched master

`AttributeAuthority::generateDecryptionKeySegments` (src/attribute-authority.cpp,
DKEY discovery path) on `master`:

```cpp
// the freshness period should be configurable, but this value shouldn't affect much
auto dkSegments = m_segmenter.segment(dkSpan, objName, m_maxSegmentSize, 4_s);
```

The patch changes this to `0_s`.  Chain under NDN semantics:

1. DKEY discovery Interest is unversioned and `MustBeFresh`;
2. the authority publishes all DKEY content under that same discovery name
   with `FreshnessPeriod = 4 s`;
3. a grant-only Controller-version advance re-publishes a *replacement*
   complete DKEY for the target under the same discovery name (same ABE
   generation, different content);
4. any NDN Content Store still holding the previous DKEY Data satisfies a
   `MustBeFresh` discovery within the 4 s window — the target receives the
   previous complete DKEY, which lacks the newly granted attribute and cannot
   decrypt post-grant ciphertext.

Upstream's own comment acknowledges the knob ("should be configurable").
Triggering requires a same-discovery-name replacement inside the freshness
window followed by a target refetch — the Spec179 unit/LocalMock surface has
no Content Store timing, and the MiniNDN grant scenarios fetch once per epoch,
so neither deterministically fires the window.  This is why third-party tests
(and earlier Spec179 component tests) saw no failure: the gap is latent under
the upstream usage pattern, and the patch removes the window rather than
fixing an always-firing bug.

## Boundary

No unpatched *behavioral* red-run was produced: the compile-contract failure
above blocks running any Spec179 test binary against the unpatched library.
The freshness gap is established by master source facts + NDN Content Store
semantics (deterministic implication), not by an observed unpatched runtime
failure.  A direct runtime repro would require a constructed two-fetch
scenario against a real CS (MiniNDN) with an in-window replacement, on top of
a separately maintained legacy NDNSF/NAC-ABE build.

## Evidence integrity

No failed run was discarded or rewritten.  The unpatched build and the failed
compile log were produced fresh for this record; intermediate artifacts were
removed after capture (worktree, CMake build dir, NDNSF out dir, log).
