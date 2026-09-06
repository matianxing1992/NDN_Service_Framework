# T020 reseal notice — G3 root preflight

**Status**: superseded candidate; no qualification PASS

The previous T020 G0--G2 manifests were produced before the G3 driver gained
its fail-closed root preflight. That behavior-bearing script change invalidates
the source seal and every downstream candidate derived from it.

The first invocation of the corrected G3 command was intentionally rejected
before creating a real output root when run as an unprivileged user. The
underlying MiniNDN runner requires root, so this is a setup correction rather
than a MiniNDN case result. No SIF build, upload, or Tiger job is authorized by
that attempt.

The next valid route is one new source seal, G0--G2 once, then the same checked-
in G3 driver under `sudo -E`. Passing historical manifests must not be pooled
with the new candidate.
