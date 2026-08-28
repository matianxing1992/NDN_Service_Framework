# UAV-APP MiniNDN Acceptance Contract

## Admissible Runtime

```text
MiniNDN node memphis:
  NFD
  App_ServiceController
  UavGroundStationApp

MiniNDN node ucla:
  NFD
  UavDroneApp
```

The runner MUST use `Experiments/Topology/AI_Lab.conf`, the tracked UAV runtime
and per-application configuration, and the real UAV video source. It MUST launch
the built applications through MiniNDN `getPopen`; host NFD, fake stream
objects, standalone probes, and quick-smoke do not satisfy this contract.

## Three Validation Levels

| Level | Duration | Purpose | Claim boundary |
|---|---:|---|---|
| Preflight | <10 s | binary/config/topology availability | no runtime or performance claim |
| Real smoke | short bounded run | verify controller, Drone, GS, video decode, and predictive path wiring | functional only |
| Formal cell | ≥60 s measured after readiness/warm-up | complete traffic, reliability, and latency evidence | supports measured claims |

## New-Binary Rule

The new binary exposes only Predictive streaming. It MUST NOT select
Mapping-first through a command-line flag or environment variable. If an
old/new comparison is requested, the runner receives two explicit immutable
subject manifests:

```text
old subject = pinned pre-migration commit + binary SHA-256
new subject = Spec 148 commit/tree + binary SHA-256
```

No old high-level API is retained in the new binary for benchmarking.

## Required Runtime Markers

Provider/Drone:

```text
STREAM_API_ACTIVE role=provider mode=predictive stream=<...> epoch=<...>
STREAM_PUSH stream=<...> sequence=<...> wire_sha256=<...>
STREAM_FLUSH stream=<...> group=<...> sources=<...> repairs=<...>
STREAM_API_STOP role=provider stream=<...>
```

Consumer/Ground Station:

```text
STREAM_API_ACTIVE role=consumer mode=predictive stream=<...> epoch=<...>
STREAM_FUTURE_INTEREST stream=<...> sequence=<...>
STREAM_ITEM_ADMITTED stream=<...> sequence=<...> provenance=<...>
STREAM_API_STOP role=consumer stream=<...>
```

Exact spelling may be centralized in a shared telemetry helper, but each field
is mandatory. A run fails if either role is absent, if the mode is not
Predictive, if the logged descriptor/session identities disagree, or if an old
Mapping-first high-level marker is present.

## Formal Cells

### Cell A — Zero loss

- fixed `memphis`↔`ucla` topology and tracked video/configuration;
- 5–10 second warm-up after the ready marker;
- at least 60 seconds measured;
- no experiment netem loss, delay, or reorder;
- prove stable delivery, future hits, exact-wire integrity, and zero separate
  Payload-layer Interests.

### Cell B — Fixed light loss/reordering

- identical to Cell A except one frozen netem profile;
- profile recorded before NFD traffic and verified from qdisc evidence;
- validate repair first, bounded retry second, and explicit terminal gaps;
- do not tune window, retry, timeout, FEC, SVS timing, or application workload
  after seeing the result.

## Required Metrics

Each peer and aggregate summary MUST report:

```text
attempted/pushed/flushed/admitted/delivered
delivery ratio
AoI/end-to-end latency mean, p50, p95, p99
longest delivery gap
future Interests and future-hit ratio
Mapping and Payload Interest counts
retry, timeout, Nack counts
repair attempts, recoveries, and recovery ratio
terminal gaps/skips by reason
useless Interest count and ratio
```

## Required Artifacts

```text
manifest.json
command.txt
environment.json
hashes.json
topology/config copies or hashes
controller.log
drone.log
ground-station.log
NFD/PIT observations
cell-summary.json
cell-summary.csv
analysis.md
```

The analyzer exits nonzero for missing processes/markers/artifacts, incomplete
metrics, new-binary dual-path evidence, hash/config mismatch, a measurement
window shorter than 60 seconds, or zero delivered video.
