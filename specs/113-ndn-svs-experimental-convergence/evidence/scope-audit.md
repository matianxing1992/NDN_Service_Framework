# Review Surface Scope Audit

**Date**: 2026-07-15  
**Baseline**: pinned `origin/master` at `db9fc25`

| Path | Owner / justification |
|---|---|
| `ndn-svs/mapping-provider.cpp` | sparse/partial mapping response |
| `tests/unit-tests/mapping-provider.t.cpp` | mapping behavior tests |
| `ndn-svs/fetcher.cpp/.hpp` | bounded fetch retry and callback lifetime |
| `ndn-svs/mapping-provider.hpp` | recovery fetch-window control |
| `ndn-svs/svsync-base.cpp/.hpp` | prepared store transaction and recovery controls |
| `ndn-svs/tlv.hpp` | Repair protocol TLV |
| `ndn-svs/store.hpp`, `store-memory.hpp` | publication rollback capability and implementation |
| `ndn-svs/svspubsub.cpp/.hpp` | shared mapping, recovery, segmentation, and publication transaction owner |
| `tests/unit-tests/svspubsub.t.cpp` | focused behavior and failure tests |
| `wscript` | fork Boost 1.71 configure baseline |

`svspubsub` is intentionally shared across three concerns and must be split by
hunk during final commit reconstruction. No unrelated NDNSF, UAV, Repo,
container, workflow, result, security-options, or custom timestamp path remains.

Checks:

- product `.gitignore` delta: zero;
- reviewed security-options/test delta: zero;
- unresolved conflict markers: zero;
- `git diff --check`: pass;
- the local high-loss design remains ignored and outside product status, with
  an immutable backup copy.
