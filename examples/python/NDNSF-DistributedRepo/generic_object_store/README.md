# Historical Generic Object Store Example

This Python server/client campaign belongs to the pre-Spec-192 implementation.
Its Python Repo server, client orchestration, catalog sidecar and repair policy
were retired. The scripts now exit with `SPEC192_COMPATIBILITY_V1`.

Current ownership:

- Generic objects use the C++ `RepoNode` API.
- Persistent network service uses the C++ `DistributedRepoServer`.
- Immutable files use `ArtifactRepositoryApi`; Python calls are thin bindings
  over that native API.
- Legacy state import is an explicit offline `ndnsf-repo-import` operation.

Historical experiment definitions remain under `Experiments/`, but they are
not current qualification evidence and must be migrated before use.
