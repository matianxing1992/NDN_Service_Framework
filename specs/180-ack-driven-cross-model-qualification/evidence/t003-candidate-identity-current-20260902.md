# Spec180 T003 Candidate Identity Evidence

**Status**: PASS for the shared identity library; no runtime or deployment
qualification is implied.

## Reproduction

```bash
python3 -m pytest -q tests/python/test_spec180_candidate.py
```

Result: `6 passed`.

## Verified contract

- `CandidateRecord` requires all ten contract planes and a canonical digest for
  every plane.
- `sourceDelta.dirty` and its modified/untracked paths are explicit; a dirty
  candidate without paths and a clean candidate with dirty paths fail closed.
- Canonical JSON ordering makes equivalent map order produce one identity
  digest.
- Evidence must carry the exact candidate digest and optional matching ID;
  evidence from another candidate is rejected.
- A changed plane returns the changed-plane list and the earliest restart gate
  from the contract invalidation matrix.

`scripts/spec180_candidate.py` is the sole identity implementation. Later
planning, local-gate, SIF, and Tiger tooling must consume it rather than define
parallel hashes. The module has no network, scheduler, filesystem mutation, or
model-loading side effects.
