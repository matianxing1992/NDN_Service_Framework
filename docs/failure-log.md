# NDNSF Failure Log

## 2026-09-05 — online authorization audit detects censored MiniNDN failures
- **Area**: Spec179 MiniNDN evidence and exit status
- **Symptom**: a success plus a failed request while providers were alive was counted as one successful row; a completed run with gatePassed=false returned exit code 0.
- **Root cause**: the grant collector used the earliest provider log timestamp as a termination cutoff and dropped failure rows; main checked process completion alone.
- **Fix**: retain all terminal rows, allow bootstrap/workload/drain in the grant scenario lifetime, and require gatePassed=true for exit 0. Two regression cases reproduced both defects before the fix; the 11-case launcher suite passed afterward.
- **Ref**: tests/minindn/test_request_scoped_confidentiality.py; /tmp/spec179-online-auth-harness-red.log and harness-green.log; T018.
- **Lesson**: process completion is not a security gate; never infer teardown from a first log or silently discard negative evidence.

## 2026-09-05 — online authorization audit preflight and test authoring corrections
- **Area**: Context Mode and Controller regression fixture
- **Symptom**: active authority hashes were stale; project query guard rejected a low-entropy identifier and then an identifier absent from its query; a new C++ regression did not compile.
- **Root cause**: prior Spec edits were not indexed; malformed guard arguments; makeServiceRevocation takes const char* rather than std::string.
- **Fix**: reindexed canonical authority documents, verified active health, corrected and reran the guarded project query; passed the temporary URI through c_str for the immediate copying helper call.
- **Lesson**: use file-backed checkpoints after retrieval failures and verify fixture signatures before writing a regression. An initially rejected query is not accepted authority.

Append-only engineering failure record. Rule (AGENTS.md): every failure that
costs non-trivial debugging MUST be appended here **in the same checkpoint
commit that fixes or records it**. New tasks MUST read the recent entries as
part of task context. Format per entry:

```text
## <date> — <one-line symptom>
- **Area**: <spec or module>
- **Symptom**: <what was observed>
- **Root cause**: <why>
- **Fix**: <what changed / workaround>
- **Ref**: <commit, evidence file, or script>
- **Lesson**: <one line to carry forward>
```

## 2026-09-05 — git index duplicate entries wrote a corrupted tree
- **Area**: tooling/git
- **Symptom**: `git add -A` with a pathspec containing a comma staged
  duplicate index entries; the resulting commit tree had `duplicateEntries`
  + `treeNotSorted` (`git fsck` errors), and a rename-detection warning
  "duplicate destination".
- **Root cause**: comma pathspec left the index with unordered/duplicate
  stage entries.
- **Fix**: `rm .git/index && git reset --mixed <last-good>` rebuilt the
  index; re-staged with explicit paths; `git prune --expire=now` dropped
  the bad commit.
- **Ref**: NDNSF commits `32b1fc23` (re-created) replacing the bad
  `8fc879ce`.
- **Lesson**: never use `git add -A` with comma/odd pathspecs; verify
  `git ls-files | sort | uniq -d` is empty before committing.

## 2026-09-05 — stale campaign-summary.tsv contradicted final MiniNDN results
- **Area**: Spec179 evidence
- **Symptom**: `results/spec179-minindn/campaign-summary.tsv` showed many
  scenarios `gatePassed=False` while per-scenario `result.json` said true.
- **Root cause**: the TSV predated the final campaign runs (13:08 vs runs
  15:47–17:18) and was never regenerated.
- **Fix**: regenerated from the final `result.json` files (14/14
  `gatePassed=True`).
- **Ref**: Spec179 `evidence/post-implementation-audit.md` R179-A4.
- **Lesson**: derived summary artifacts must carry a timestamp and be
  regenerated, or deleted, after the runs they summarize.

## 2026-09-04 — MiniNDN campaign caught two admission defects
- **Area**: Spec179 runtime
- **Symptom**: S9 — `RequestServiceTargeted` issued versionless requests;
  S10 — `requestServiceStreamingBytes` discarded the admission result so a
  denied stream start logged STARTED.
- **Root cause**: missing version binding on the Targeted request path;
  ignored revocation admission result on the stream start path.
- **Fix**: fixed in `ServiceUser.cpp`; rebuilt; genuine campaign rerun
  green.
- **Ref**: `evidence/minindn-campaign-20260904.md`, NDNSF commit `e7ea0a74`.
- **Lesson**: the cross-process campaign is the admission-boundary oracle;
  component tests did not catch either defect.

## 2026-09-03 — OpenABE mixed-generation decrypt returns garbage
- **Area**: NAC-ABE/OpenABE crypto
- **Symptom**: decrypting new-generation ciphertext with a retained old
  DKEY could return garbage plaintext instead of throwing.
- **Root cause**: OpenABE generation mismatch does not always fail loudly.
- **Fix**: Spec179 test assertions use `decryptFailsClosed` (throw **or**
  recovery failure both accepted); RV-U20 mixed-generation matrix with
  fresh ciphertext per case (the ABESupport singleton CK cache would
  otherwise mask the mismatch).
- **Ref**: Spec179 `evidence/runtime-revocation-lifecycle-20260904.md`.
- **Lesson**: crypto-negative assertions must accept "recovered garbage"
  as failure; never reuse a successfully-decrypted ciphertext in a
  generation-mismatch case.

## 2026-09-03 — NAC-ABE stale DKEY after grant-only policy replacement
- **Area**: NAC-ABE dependency
- **Symptom**: target refresh could receive the previous complete DKEY.
- **Root cause**: DKEY segments published with `FreshnessPeriod=4s`; the
  unversioned `MustBeFresh` discovery Interest then hit a still-fresh
  Content Store copy of the old policy.
- **Fix**: DKEY segments now publish with `FreshnessPeriod=0`; exact
  versioned segment names remain retrievable.
- **Ref**: NAC-ABE `Experimental` branch commit `b1c9c4f` (not pushed).
- **Lesson**: any in-place policy replacement needs freshness discipline
  on unversioned discovery names.

## 2026-09-03 — versioned exact public-params Interest could never match
- **Area**: NAC-ABE dependency
- **Symptom**: after status installation,
  `refreshPublicParameters` with the exact
  `/PUBLIC-PARAMS/<ABE-TYPE>/v=<version>` name (CanBePrefix=false) timed
  out repeatedly.
- **Root cause**: `AttributeAuthority::onPublicParamsRequest`
  unconditionally appended `<ABE-TYPE>` + version to the Interest name,
  producing a Data name that can never satisfy the exact request.
- **Fix**: detect an already-versioned name and do not append again;
  ParamFetcher binds expected name/digest.
- **Ref**: NAC-ABE `Experimental` commit `b1c9c4f`.
- **Lesson**: producer-side name derivation must mirror every Interest
  shape the consumer may legally send.

## 2026-09-02/04 — build and test-environment traps (Spec179 baseline)
- **Area**: build/tests
- **Symptom** (three independent traps):
  1. GCC 9 ICE on `data-enc-dec.cpp` — NAC-ABE must be built with
     `clang++-10`.
  2. Two concurrent waf builds in different out dirs conflict on the
     shared lock and one is killed silently.
  3. After a full-suite SIGSEGV, Boost.Test keeps running and the
     residual process disturbs later timing runs — kill residuals before
     re-running.
- **Fix**: documented build recipe (clang++-10, single build at a time,
     kill-then-retest).
- **Ref**: Spec179 `evidence/restore-fixes-20260904.md`,
     `evidence/regression-red-green-20260904.md`.
- **Lesson**: environment traps must be recorded next to the build
  recipe, not rediscovered per session.

## 2026-09-02 — DummyClientFace hangs and LocalMock DKEY reattach
- **Area**: tests
- **Symptom**: `processEvents` blocked forever on a fully idle face;
  pump-driven LocalMock members could not verify DKEY segments.
- **Root cause**: deferred DKEY reattach had no bound when the face went
  idle.
- **Fix**: bounded retry (250 ms × 20) for deferred DKEY reattach;
  request-pump fixture extended to pump the AA face (Spec179 remounts).
- **Ref**: Spec179 baseline fixes in NDNSF commit `e7ea0a74`.
- **Lesson**: every deferred async retry needs a bounded schedule or an
  idle-face test can deadlock the whole suite.

## 2026-09-02 — SegmentFetcher infinite fetch on discovery Data
- **Area**: Core `ServiceProvider::replyFromIMS`
- **Symptom**: SegmentFetcher kept requesting segments until timeout.
- **Root cause**: discovery Data served from IMS lacked `FinalBlockId`.
- **Fix**: forward to the last contiguous IMS segment and set
  `FinalBlockId`.
- **Ref**: Spec179 baseline fixes.
- **Lesson**: any segmented Data served to a SegmentFetcher must carry a
  terminal marker or the fetch is unbounded.
