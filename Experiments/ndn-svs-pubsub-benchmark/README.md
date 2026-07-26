# Spec 131 Pure NDN-SVS PubSub Peer

This external driver is compiled twice, once against each immutable NDN-SVS
worktree. `SPEC131_LATEST=0` contains only synchronous `publish()` calls.
`SPEC131_LATEST=1` contains `publishAsync()` and explicitly enables the
four-worker receive/production pools. It neither links nor launches NDNSF.

Use `../build_svs_pubsub_commit_bench.py` to create both binaries and the
machine-readable build authority in `build/spec131/subjects.json`. Do not run
that script after a formal campaign manifest has been sealed.
