# Current SIF reuse gate — 2026-08-19

This is the post-cleanup identity check for the currently available local SIF.
It records physical usability separately from source parity.

## Observed state

```text
working directory: /home/tianxing/NDN/ndn-service-framework
HEAD:             989a9daace669a4f93496dade3176c527edb2469
working tree:     dirty; uncommitted/generated entries are outside r23
filesystem:       177G total, 95G used, 75G available, 56% used
SIF:              .codex-tmp/spec170-container-build-20260818-r14/runtime-r23.sif
SIF bytes:        4442767360
SIF SHA-256:      5b8bd6baaaf7288b3b593538b7c7bfa086feeca91276bef56d7b6c03e5ae9eeb
record:           .codex-tmp/spec170-container-build-20260818-r14/build-record-r23.json
sourceRevision:   989a9daace669a4f93496dade3176c527edb2469
Apptainer:        /opt/apptainer/1.5.3/bin/apptainer (1.5.3)
```

The available space increased after the explicitly recorded Apptainer-cache
cleanup in `storage-cleanup-20260819.md`. No active SIF, base SIF, source
checkout, or canonical evidence was removed.

## Cheap reuse-gate result

| Check | Result |
|---|---|
| `sha256sum runtime-r23.sif` | PASS; matches the build record |
| `validate-local-sif-build-record.py --metadata-only` | PASS; `ndnsf-local-sif-build-v3` |
| exact-SIF `/opt/venv/bin/python --version` | PASS; Python 3.10.18 |
| exact-SIF `import ndnsf, ndnsf._ndnsf` | PASS; `NDNSF_SIF_IMPORT_OK` |

This proves that r23 is physically usable for the sealed source revision. It
does **not** qualify the dirty working tree, later C++/Python/NDN-SVS/NDN-CXX
changes, a new lock or definition, or Tiger promotion. Any such change gets a
new candidate ID and a new complete local SIF.

## Required next action after an NDNSF-DI update

Do not copy a host extension, patch r23, or create a temporary SIF. Repeat the
documented order: disk/path census → candidate/source/lock/definition identity
→ target-node Apptainer parity → source/build-boundary and complete Waf target
census → one local `build-local-sif.sh` build → exact-SIF ABI/library/runtime
closure → local real lifecycle and negative gates → one hash-bound promotion.
TigerCluster remains verify-hash, stage-once, and execute-only.
