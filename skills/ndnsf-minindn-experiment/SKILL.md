---
name: ndnsf-minindn-experiment
description: NDNSF MiniNDN experiment patterns — topology, measurement, env vars, latency comparison
metadata:
  short-description: "Mandatory NDNSF MiniNDN experiment skill"
  强制性: true
---

# NDNSF MiniNDN Experiment Skill

This skill MUST be used before writing or modifying ANY NDNSF MiniNDN
experiment script. It encodes patterns from 50+ experiments in
`Experiments/` and Mini-NDN upstream examples. Both Claude Code and CODEX
use the same shared skill directory.

## 1. Experiment Skeleton

默认使用 **Minindn (有线)** 或 **MinindnWifi (WiFi 基础设施)**。MinindnAdhoc 仅用于明确需要自组网的场景。

| Class | 优先级 | 路由 | 面 |
|-------|--------|------|-----|
| `Minindn` | **默认有线** | `NdnRoutingHelper` (NLSR) | 自动 |
| `MinindnWifi` | **默认 WiFi** | 单播 `Nfdc.createFace()` | IP 直连 |
| `MinindnAdhoc` | 仅自组网 | 组播 `[01:00:5e:00:17:aa]` | `Nfdc.getFaceId(..., "ether", 6363)` |

### Wired — `Minindn`

```python
from minindn.minindn import Minindn
from minindn.apps.app_manager import AppManager
from minindn.apps.nfd import Nfd
from minindn.helpers.ndn_routing_helper import NdnRoutingHelper
from minindn.util import getPopen

ndn = Minindn(topoFile=args.topology_file)
ndn.start()
AppManager(ndn, ndn.net.hosts, Nfd)
NdnRoutingHelper(ndn, ndn.net.hosts, args.controller_node)
# ... run apps with getPopen ...
ndn.stop()
```

### Infrastructure WiFi — `MinindnWifi` (默认)

```python
from minindn.wifi.minindnwifi import MinindnWifi
from minindn.apps.app_manager import AppManager
from minindn.apps.nfd import Nfd
from minindn.helpers.nfdc import Nfdc

ndn = MinindnWifi(topoFile=args.topology_file)
ndn.start()
AppManager(ndn, ndn.net.stations, Nfd)

# 单播面 — MinindnWifi 用 IP 直连，不用组播
faceID = Nfdc.createFace(a, b.IP())
Nfdc.registerRoute(a, "/prefix", faceID)

# ... run apps with getPopen ...
ndn.stop()
ndn.cleanUp()
```

### Ad-Hoc WiFi — `MinindnAdhoc` (仅自组网/UAV)

```python
from minindn.wifi.minindnwifi import MinindnAdhoc
from minindn.helpers.nfdc import Nfdc

ndn = MinindnAdhoc(topoFile=args.topology_file)
ndn.start()
AppManager(ndn, ndn.net.stations, Nfd)

# 组播面 — MinindnAdhoc 无 AP，必须手动组播
MCAST = "[01:00:5e:00:17:aa]"
faceId = Nfdc.getFaceId(station, MCAST, None, "ether", 6363)
Nfdc.registerRoute(station, "/prefix", faceId, cost=100)

ndn.stop()
ndn.cleanUp()
```

## 2. Topology Files

Located in `Experiments/Topology/`. Format:
```
[node-name]:   _ radius=<m> angle=<deg>      # wireless
[node-name]:   ip=<addr>                      # wired
[links]:       node-a:node-b delay=<ms> loss=<pct>
```

## 3. App Launch Pattern

Always use `getPopen` from `minindn.util`:

```python
def start(node, name, command, env, output_dir, processes):
    log_path = output_dir / f"{name}.log"
    log_file = log_path.open("wb")
    proc = getPopen(node, command, envDict=env, shell=True,
                     outFile=log_file, errFile=log_file)
    processes.append((name, proc, log_path))
```

## 4. Environment Variables

Pass feature flags through env vars, not command-line args, to keep the
MiniNDN harness independent of NDNSF app internals:

```python
env = make_env(args, node_name, home_dir)
env["NDNSF_UAV_DISCOVERY_MODE"] = args.discovery_mode
env["NDNSF_TIMELINE_TRACE_SAMPLE_RATE"] = "0.01"
env["NDNSF_SVS_PARALLEL_SYNC"] = "1"
```

Common NDNSF env vars:
- `NDNSF_UAV_DISCOVERY_MODE` — "mapping-first" or "predictive"
- `NDNSF_TIMELINE_TRACE_SAMPLE_RATE` — sampling rate (e.g. "0.01")
- `NDNSF_SVS_PARALLEL_SYNC` / `NDNSF_SVS_PARALLEL_WORKERS` — SVS tuning
- `NDNSF_SVS_PUBLICATION_FETCH_WINDOW` — fetch window size
- `NDNSF_ENABLE_NDNSD` — service discovery toggle

## 5. Measurement Rules

- **Measurement window**: 60 seconds minimum for performance tests.
  Use `--auto-stop-seconds 60` or equivalent.
- **Warm-up**: Discard first 5-10 seconds of data (routing convergence).
- **Steady state**: Only measure after `NDNSF_UAV_GUI_MININDN_READY` or
  equivalent ready marker.
- **Sampling**: Use `NDNSF_TIMELINE_TRACE_SAMPLE_RATE` to keep logs small.
- **Output**: Each node's stdout+stderr to `output_dir/<node_name>.log`.

## 6. Latency Collection

Parse structured markers from node logs. Common markers:
```
NDNSF_UAV_GUI_MININDN_READY
GS_DECODED_FRAMES count=N frame=N latency_ms=X
GS_STATUS Video stopped, packets=<n>, fec_groups=<n>
DRONE_STATUS video streaming
NDNSF_UAV_GUI_MININDN_QUICK_SMOKE_OK
```

Helper for extracting latency:
```python
def parse_latency(log_path):
    import re, statistics
    lats = []
    for line in open(log_path):
        m = re.search(r"latency_ms=(\d+\.?\d*)", line)
        if m: lats.append(float(m.group(1)))
    if not lats: return {}
    return {
        "count": len(lats),
        "median_ms": statistics.median(lats),
        "p95_ms": sorted(lats)[int(len(lats) * 0.95)],
    }
```

## 7. Matched Comparison Pattern

For A/B comparisons (e.g. baseline vs treatment):

1. **Same topology** — identical topology file
2. **Same source** — identical video/input
3. **Same duration** — identical measurement window
4. **Different flag** — only the independent variable changes
5. **Separate output** — different `--output-dir` per run
6. **Post-process** — compute statistics from each log, compare

```python
# Run baseline
subprocess.run([sys.executable, script, "--output-dir", "results/baseline",
                "--discovery-mode", "mapping-first"])

# Run treatment
subprocess.run([sys.executable, script, "--output-dir", "results/predictive",
                "--discovery-mode", "predictive"])

# Compare
baseline = parse_latency("results/baseline/gs.log")
treatment = parse_latency("results/predictive/gs.log")
reduction = baseline["median_ms"] - treatment["median_ms"]
print(f"Median latency reduction: {reduction:.1f} ms")
```

## 8. Quick Smoke Test

Every experiment MUST support a fast smoke test mode:
- `< 10 seconds` duration
- Tests connectivity + basic functionality
- Does NOT produce performance claims
- Marker: `SMOKE_OK` in output

## 9. Argument Conventions

```python
parser.add_argument("--topology-file", default=str(DEFAULT_TOPOLOGY))
parser.add_argument("--output-dir", default=str(REPO / "results/<name>"))
parser.add_argument("--nfd-log-level", default="WARN")
parser.add_argument("--discovery-mode", default="mapping-first",
                    choices=("mapping-first", "predictive"))
parser.add_argument("--auto-video-test", action="store_true")
parser.add_argument("--auto-stop-seconds", type=int, default=10)
```

## 10. Common Pitfalls

- **DO NOT** use host NFD for final validation — MiniNDN only
- **DO NOT** change SVS timing casually — `suppressionInterval` stays 1-5ms
- **DO NOT** claim performance from smoke tests — smoke = connectivity only
- **DO NOT** mix measurement results from different topologies
- **DO NOT** run `ndn.stop()` before all subprocesses exit — data loss
- **DO** redirect both stdout and stderr to the same log file
- **DO** unset GStreamer/X11 env vars when running headless:
  `unset DISPLAY && unset GST_PLUGIN_PATH`
