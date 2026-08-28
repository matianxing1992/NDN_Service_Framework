# MiniNDN Validation Evidence

**Date**: 2026-07-15/16  
**Validated NDN-SVS HEAD**: `c34c04d766836bba1567a70bae846dfbd9d25b66`  
**Rule**: each declared acceptance cell was run exactly once per immutable
candidate. Failed cells were preserved and never rerun under the same identity.

## Final Passing Candidate

Candidate directory:

```text
results/spec112-segmented/spec112-1682ae9c60949343b7a5/
```

| Artifact | Identity |
|---|---|
| Candidate ID | `spec112-1682ae9c60949343b7a5` |
| Candidate identity SHA-256 | `1682ae9c60949343b7a5425c351e91fd1d9a43f830941e60c6a3d7b182854f8c` |
| `candidate-manifest.json` | `28db09885aa65958edafb01f489701d844545d84f5df13a6dfa43ecc02e89f0d` |
| `campaign-summary.json` | `dd90ff6ae5cbc353d717684dd24e2a262090ab889304427d2f0a42bd65c52853` |
| `campaign-cells.csv` | `69d046dceb01b18eeb2979416923388b9126effeecbded7a3fc15f95036aa521` |

The manifest binds pinned `origin/master`, validated review HEAD, permanent
backup, expected final local topology, exact six-cell configuration, campaign
scripts/topology, source state, binaries, Python extension, and installed versus
workspace NDN-SVS headers/library. `dependencyInstallation.ndnSvs.verified` is
`true`.

## Acceptance Results

| Cell | Result | Acceptance detail |
|---|---|---|
| `boundary-async-normal` | SUCCESS | 6/6 byte-exact: 64, 4000, 5000, 6500, 8000, 16000 B |
| `boundary-async-targeted` | SUCCESS | 6/6 byte-exact at the same sizes |
| `boundary-sync-normal` | SUCCESS | 6/6 byte-exact at the same sizes |
| `boundary-sync-targeted` | SUCCESS | 6/6 byte-exact at the same sizes |
| `burst-async-normal` | SUCCESS | 102/102: 80×8000 B, 10×64 B, 12×4000 B in one provider epoch |
| `targeted-degraded-timeout` | SUCCESS | established response 152.156 ms; degraded request timed out at 4011.192 ms under 4500 ms limit |

The four boundary cells therefore passed 24/24. The burst provider reported
`restartCount=0`. Every normal cell ended with the provider alive. All six
cells reported no wall/disk stop, no large-data reference marker, no
oversized-packet/crash marker, and no unexpected duplicate terminal callback.
The degraded request emitted exactly one timeout terminal and zero response
terminals; terminating the provider is the intended fault, so provider liveness
is not required for that cell.

The manifest was reloaded and the candidate identity recomputed successfully
immediately after all cells and before this closeout document changed the root
repository input tree. Closeout documentation is therefore not used to create
or silently replace the frozen candidate.

## Preserved Failed Candidates And Diagnosis

Two formal candidates failed and remain part of the evidence:

| Candidate | Cell | Measured outcome |
|---|---|---|
| `spec112-5511f66238a4f08f22d7` | `boundary-async-normal` | FAILURE, 0/6, user wall stop, provider remained alive |
| `spec112-98faafea4c1b5743c3e5` | `boundary-async-normal` | FAILURE, 0/6, user wall stop, provider remained alive |

One non-acceptance diagnostic cell,
`spec112-5511f66238a4f08f22d7/diagnostic-constructor-stack`, reproduced the first
failure. GDB located the block in `SVSyncCore::setMaxSuppressionTime`. The root
cause was not a newly observed publication-runtime defect: NDNSF objects were
compiled using stale `/usr/local/include/ndn-svs` headers while loading the new
workspace shared library. The first root clean build did not correct this
because `pkg-config` continued to select the stale installation.

Installing the committed headers/library, rebuilding NDNSF unit/shared-library
targets and the Python extension, and adding manifest equality checks removed
the ABI mismatch. No failed candidate or cell was rerun or rewritten; the
passing result has a new candidate identity.

## Verdict

T046-T050 pass. The final committed NDN-SVS candidate meets all Spec 112
segmented-response, same-epoch health, and Targeted-timeout acceptance counts
under MiniNDN, with source/binary/dependency identity bound to the evidence.
