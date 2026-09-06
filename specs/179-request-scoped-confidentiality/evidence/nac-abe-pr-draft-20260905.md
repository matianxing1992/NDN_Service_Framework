# Preserve authorization boundaries across key refresh and cache reuse

Proposed draft PR destination: `matianxing1992/NAC-ABE`, base `master`,
head `Experimental` at `85547eb558c4a4f706b51cb354ab4db573609895`.
Publication is pending authorization. The body below describes the complete
four-commit change against `1cc17d9`, not a cherry-pick onto original-project master.

---

A grant-only policy replacement can return a cached DKEY with the previous
attributes. During invalidation, delayed fetch/validation callbacks can also
restore obsolete material, and a callback clearing the Consumer cache can
invalidate the active completion batch. Separately, a warm decrypted-key cache
can incorrectly reuse an authorized result for a different caller key.

This change makes DKEY discovery immediately stale, adds explicit authority
policy/generation operations and exact parameter binding, and fences asynchronous
work by generation. It detaches callback batches before calling applications,
binds decrypted-key cache entries to the complete cryptographic context, and
prevents current parameters being relabeled with a requested old version.
Authorization decisions remain with the application Controller.

Existing ordinary source calls remain supported, including the no-argument
parameter-fetch member. Public class layouts change: all dependent libraries,
executables and language extensions require a clean matching rebuild. Consumer
invalidation cancels callbacks silently; the application owns cancellation or
timeout notification. See `docs/experimental-compatibility.md` for Face-thread,
object-lifetime, serialized OpenABE worker and rollback requirements.

Validation on the complete local revision: NAC42/42 cases with3299 assertions,
three example builds, and installed-prefix20/20 cases. A clean matched NDNSF
build passes183 unit and72 integration cases. Its final18-scenario MiniNDN
Controller grant/revoke campaign passes188 assertions, both dedicated User
grant gates, and33 artifact-hash checks. Earlier failures were retained in the
NDNSF Spec179 evidence. These are configuration-specific correctness results;
they do not establish universal compatibility or performance.
