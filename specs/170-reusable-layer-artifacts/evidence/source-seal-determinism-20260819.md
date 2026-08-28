# Source-seal determinism regression (2026-08-19)

## Finding

The source-only preparation tool produced identical workspace and NDN-SVS
archive hashes in two output directories, but its `sealDigest` previously
changed because the JSON digest included absolute `workspace` and archive
paths. That made a content-identical source look like two different
candidates after relocation.

## Fix and compatibility boundary

`prepare-local-sif-source.py` now emits
`sealDigestBasis=path-independent-content-v1` and hashes a canonical body in
which operational paths are replaced by stable labels. The validator applies
that basis to new seals and continues to validate legacy path-bound v1 seals.
No existing SIF or source seal was rewritten.

## Evidence

The current dirty workspace and dirty NDN-SVS checkout were prepared twice in
separate temporary directories:

```text
workspace files:       294
NDN-SVS files:          31
workspace archive:     sha256:7975e10242e6154b6d9c236b869f76989d95dc5d14f912fd7e05b73c2d945916
NDN-SVS archive:       sha256:1136e464ef6ec3fcd154199216d5d8d46c3ac7deb1c8a354c924dfca7223a5cd
canonical seal:        sha256:ff5d13fc2e82a68cdbd6b9d0a661b1be67cc326e2bcbafb44d868063246ed627
archive hashes equal:  PASS
file rows equal:       PASS
seal digests equal:    PASS
both validators:       PASS
```

The archive is a source-preparation probe, not a T029 freeze candidate. The
working tree still requires deliberate source review, baseline regeneration,
Gate A/B/C closure, and a new local SIF before Tiger execution.

## Regression tests

```text
python3 -m pytest -q \
  tests/python/test_prepare_local_sif_source.py \
  tests/python/test_build_local_sif_record.py
8 passed in 1.29 s
```
