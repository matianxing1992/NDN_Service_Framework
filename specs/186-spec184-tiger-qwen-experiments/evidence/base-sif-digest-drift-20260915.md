# Spec186 base SIF identity drift

The r67/r68/r69 handoff locks recorded the temporary base
`spec186-repaired-base-final.sif` as
`sha256:1dd9626748b6fdbe93abf819a944e0628bf7a2b5feddcc562fe7d233f927e74c`.
Before r69 definition rendering, the actual file at the same path was hashed
again and measured as:

| Field | Observed |
| --- | --- |
| path | `.codex-tmp/spec186-repaired-base-final.sif` |
| bytes | `3088814080` |
| actual SHA-256 | `sha256:071bae8eed882bf865f4f5f6bf06c900f17a93e42bac870001a331c9e2cc82c2` |
| stale lock SHA-256 | `sha256:1dd9626748b6fdbe93abf819a944e0628bf7a2b5feddcc562fe7d233f927e74c` |
| exact render result | rejected with `HANDOFF_BASE_DIGEST` |
| Apptainer inspect | blocked by local mount hook (`destination /dev doesn't exist in container`); `apptainer sif list` still sees one SquashFS partition |

The old identity is not reused. A subsequent handoff must update the lock to
the actual base bytes only after the base is independently accepted as the
intended template-derived input. No SIF build or runtime qualification is
implied by the digest refresh.
