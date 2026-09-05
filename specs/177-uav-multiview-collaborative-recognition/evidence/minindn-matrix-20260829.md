# Spec 177 MiniNDN multi-process evidence

**Run date**: 2026-08-29  
**Launcher**: `NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py`  
**Mode**: real MiniNDN/NFD execution under `sudo -n`  
**Topology**: one `gs` router/coordinator, one controller, four UAV image
producers, and two competing compute Providers (`/provider/gpu`,
`/provider/cpu`)  
**Wire payload rule**: request, ACK, selection, and response payloads contain
references/metadata only; image bytes are served as named Data by UAVs.  
**Scientific accuracy claim**: not allowed; this is a transport/lifecycle gate.

## Command

```bash
sudo -n python3 NDNSF-UAV-APP/tools/run_uav_multiview_minindn.py \
  --execute --all-scenarios --provider /provider/gpu \
  --output <campaign-directory>
```

The launcher generated 64px deterministic PNG derivatives for the wire gate
because the checked-in source images exceed the default MiniNDN UDP packet
path.  Each derivative has its own content digest and the job retains the
corresponding source-fixture digest.

## Matrix result

| Scenario | Terminal state | Selected owner | Gate |
| --- | --- | --- | --- |
| nominal | `completed` | `/provider/gpu` | PASS |
| provider-selection | `completed` | `/provider/gpu` | PASS |
| unavailable-view | `rejected` (`delivery-timeout`) | `/provider/gpu` | PASS |
| late-view | `rejected` (`delivery-timeout`) | `/provider/gpu` | PASS |
| publication-failure | `rejected` (`annotation-publication-failed`) | `/provider/gpu` | PASS |

All five runs returned `gatePassed=true`.  The nominal and selection runs each
verified six exact UAV Data packets, six Provider-owned annotation Data
packets, one Provider-owned result Data packet, and one terminal owner.  The
failure runs recorded the injected missing/late view or annotation publication
failure and did not emit a successful terminal result.

## Retained result hashes

The latest recheck per-scenario `result.json` SHA-256 values were:

```text
nominal              f26a768c36f62b984a11e05535bfca33b070e6b60acdfec1de1921600998fefd
provider-selection   d62d980813bfde55846c98f0e425d099c59116810c6dc9dfabf9e821164ea131
unavailable-view     a9cf9b1f36cbad5b4f29dae99415e3a35a29e610a6ab7b261c0d872581fa217c
late-view            61a931f0ad86e5225117c10df24551f0d5535539f54f02388d8a0c70facfc066
publication-failure  c4f824333da4385dbc51d43814dad23e316a31b5e52187192a89f590efa8391f
```

The original campaign directory is retained outside the repository working
tree; this evidence records the deterministic result hashes and the exact
matrix contract without adding generated logs or images to the source tree.
