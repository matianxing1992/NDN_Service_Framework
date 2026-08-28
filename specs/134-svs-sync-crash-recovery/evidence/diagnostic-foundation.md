# Spec 134 Diagnostic Foundation

- Exact historical base: `a9944019f76791773604999f00128057b9534ace`
- Boost-1.71-only clean head: `bf1e3e37f0c4c7a5a04d678f0fa439283ee46d2d`
- ASan/UBSan compiler: GCC 9.4
- TSan compiler: Clang 10.0 (GCC TSan development object
  `libtsan_preinit.o` is absent on this host; this limitation was not hidden)
- Subject manifest: `build/spec134/subject-manifest.json`
- Both binaries resolve only the isolated historical NDN-SVS library and Boost
  1.71 libraries.
- Contract tests: 10/10 passed via
  `python3 tests/python/test_spec134_svs_sync_crash_recovery.py`.
- The active NDN-SVS checkout, Spec 131 worktrees, and Spec 133 worktrees were
  hash-snapshotted and unchanged.
- A historical CodeGraph snapshot was created under
  `build/spec134/codegraph/historical-clean-bf1e3e37/.codegraph`; it was built
  from a read-only `git archive`, so the source NDN-SVS checkout was not
  modified.

No MiniNDN diagnosis or root-cause finding is claimed by this foundation.
