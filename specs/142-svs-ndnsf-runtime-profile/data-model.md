# Data Model: Spec 142 Experiment Evidence

## RuntimeProfileManifest

One immutable manifest is created before any formal cell.

| Field group | Required content |
|---|---|
| Source | NDNSF commit/status, NDN-SVS base commit and exact dirty diff hash |
| Build | compiler, Boost, OpenSSL, configure/build commands |
| Binary identity | benchmark path, SHA-256, ELF/RUNPATH, `ldd` |
| Library identity | resolved ndn-cxx and NDN-SVS paths and SHA-256 |
| Topology | two nodes, link bandwidth, one-way delay, configured loss |
| CPU | online CPUs, affinity, active worker pools and queue sizes |
| Security | RSA key type/size, signer and validator path |
| Workload | payload bytes, directions, pacer, target rates |
| SVS profile | V3, suppression, periodic policy, timestamp mode, piggyback limit |
| Fetch profile | Mapping and publication windows, retries, lifetimes, backoff |
| Timing | warmup, measurement, drain |
| Treatment | allowed mode delta and expected resolved values |

The manifest is content-addressed. Every `CellReceipt` references its SHA-256.

## CellReceipt

One receipt exists for every started cell.

| Field group | Required content |
|---|---|
| Identity | campaign ID, cell ID, mode, target rate, manifest SHA-256 |
| Resolved peer profile | every mode-independent SVS option and worker count |
| Processes | PIDs, exits, start/end timestamps, runtime library hashes |
| Workload | attempted count/rate per peer and pacing error |
| Delivery | delivered count/rate/ratio per peer |
| Latency | raw sample path/hash/count and mean/p50/p95/p99 |
| Wire path | signed publication size distribution, eligible/hit/fallback counts |
| Fetch health | Mapping/Publication Interest, Data, retry, timeout, Nack counts |
| Security | RSA sign/validate counts and failures |
| Resources | CPU time, utilization, thread counts, queue high-water marks |
| Classification | `PROFILE_VALID`, `PROFILE_INVALID`, or `HARNESS_FAILED` |
| Reason | deterministic list of passed/failed gates |

### Classification rules

- `PROFILE_VALID`: identity, profile, +/-2% attempted rate, process, security,
  raw accounting, and zero retry/timeout/Nack gates all pass.
- `PROFILE_INVALID`: a completed cell violates a profile, Fetch-health,
  attempted-rate, security, or accounting invariant. Preserve it as diagnostic
  evidence; do not use it in a worker-only comparison.
- `HARNESS_FAILED`: the runner cannot establish a trustworthy receipt because
  setup, process management, or artifact capture failed.

`LOAD_UNSUSTAINED` is an outcome field, not a receipt validity class. It can
coexist with `PROFILE_VALID` when the system fails to deliver all attempted
publications without activating recovery or breaking evidence invariants.

## ModeComparison

A `ModeComparison` is produced only for a target rate with one
`PROFILE_VALID` inline receipt and one `PROFILE_VALID` worker receipt.

Required outputs:

- attempted and delivered pps and delivery ratio for both peers and aggregate;
- latency mean, p50, p95, and p99 for both modes;
- absolute and relative differences;
- piggyback and Fetch-path counters;
- CPU and queue context;
- explicit statement whether delivery was complete or survivor-only;
- claim scope limited to publication preparation placement.

