# Build Evidence Model

## Layer Lock

- `schemaVersion`
- `layer`
- pinned upstream image or parent identity
- packages and exact versions
- source repositories and exact revisions
- downloaded asset URLs, SHA-256, and byte size
- exclusions

## Layer Seal

- lock SHA-256
- archive path, size, SHA-256, and revision for each source
- creation timestamp
- aggregate seal digest

## Built Image

- logical product name
- local tag
- image ID
- configuration digest
- RepoDigests, if published elsewhere
- parent image IDs
- size and creation time
- lock/seal digests
- probe status

## Build Run

- build ID and authority (`developmentCandidate` or `formalRelease`)
- workspace revision and dirty-state digest
- host/tool versions and parallelism
- ordered commands and durations
- produced images
- scan/probe results
- failure phase and reason, if any

State transition:

```text
PLANNED -> LOCKED -> BUILT -> PROBED -> ACCEPTED
                         \-> FAILED
```

Only `ACCEPTED` products can serve as parents for the next layer.
