# Implementation Plan: Four-Provider Multi-AP Mobility Advantage

**Branch**: `Experimental` | **Date**: 2026-08-05 | **Spec**: [spec.md](spec.md)

## Summary

Extend the Spec169 mobility baseline into a trace-paired four-Provider
campaign. The existing NDNSF, gRPC-HC-3, and NSC-3 evidence remains immutable.
The primary new comparison is the original one-AP, four-Provider topology:
NDNSF versus strict `gRPC-SEQ-4` and NSC-4 while coverage radius and drone
speed vary. Health-assisted `gRPC-HC-4` is retained only as an explicitly
enabled sensitivity condition.
The new campaign will make Provider cardinality, AP layout, coverage radius,
and speed explicit; use the same deterministic trace for all systems; and
measure both reliability and retry/control cost. A smoke gate comes before any
formal 60-second campaign.

## Technical Context

**Language/Version**: Python 3, C++17, existing gRPC Python client and NSC
consumer

**Primary Dependencies**: MiniNDN-WiFi, Mininet-WiFi/wmediumd, grpcio,
ndn-cxx, existing NDNSF example binaries

**Storage**: Local `results/` campaign directories; JSON/CSV manifests,
structured client summaries, mobility traces, and per-node logs

**Testing**: Python contract tests, C++ NSC focused tests, MiniNDN-WiFi smoke,
then matched 60-second campaign cells

**Target Platform**: Linux NDNSF development VM with MiniNDN-WiFi; no host NFD
for final network evidence

**Project Type**: Reproducible experiment harness and baseline clients

**Performance Goals**: Smoke measured workload under 10 seconds; formal cells
use 60 measured seconds at 5 logical requests per second and 300 requests

**Constraints**: Primary cells use a common 5-second logical deadline,
1-second NSC/gRPC attempt timeout, and 1-second NDNSF ACK timeout. A strictly
paired 2-second/5-second timeout sensitivity pilot is allowed as a separately
labelled condition; NDNSF remains FirstResponding and its ACK timeout does not
delay first-successful-ACK selection. There is no availability oracle in the
primary clients; health probing is explicit opt-in; no automatic rerun; sampled
traces; preserve Spec169 artifacts

**Scale/Scope**: Four mobile Providers, one fixed requester/controller, one
physical AP in the primary matrix, 50/100-m pilot coverage, and 2/15-m/s pilot
speed bands; multi-AP and 8-m/s cells remain secondary extensions

## Constitution Check

- **Canonical Dynamic Runtime**: PASS. No NDNSF protocol/API change is planned.
- **Security Is Part Of The Data Path**: PASS. Existing permission/token paths
  remain unchanged; the experiment only changes Provider cardinality and
  topology inputs.
- **CodeGraph First**: PASS. CodeGraph was used to locate mobility harness,
  provider topology, gRPC client, and NSC attempt state before source edits.
- **Spec-Driven Work**: PASS. This feature has its own requirements, plan,
  tasks, contracts, and evidence boundary. Spec169 remains a frozen upstream
  baseline.
- **Verify With The Right Scope**: PASS. MiniNDN-WiFi is the final network
  path; smoke is connectivity-only and formal runs use a 60-second window.
- **Cohesive Tasks**: PASS. Tasks are behavioral slices: four-Provider
  configuration, matched multi-AP trace execution, and evidence/claim gate.

## Phase 0 Research Decisions

1. Preserve Spec169: its three-Provider, one-AP coverage-gated results answer a
   different question and must not be silently relabeled.
2. Generalize Provider lists instead of cloning a second baseline client. NSC
   already parses an arbitrary Provider list; gRPC and the harness currently
   enforce three and need the smallest compatible generalization.
3. Use a deterministic nearest-AP coverage schedule first. This isolates
   multi-Provider availability and makes systems trace-paired. Report it as
   coverage-gated evidence until actual association events are instrumented.
4. Use four Provider endpoints with sequential gRPC/NSC attempts and a common
   global deadline. The primary gRPC client must not use proactive health
   routing; retain the health-assisted variant only as an explicitly enabled,
   separately labelled sensitivity condition. The experiment's current
   `NDNSFBaseline/Health` RPC is application-level and is not the standard
   `grpc.health.v1` protocol. Do not compare against single-endpoint gRPC as
   the primary baseline.
5. Register a `single-active-handoff` coverage trace in which one Provider is
   reachable at a time and the active Provider rotates on a fixed period. This
   directly tests whether one NDNSF publication can reach the currently
   reachable Provider while sequential baselines spend the same deadline on
   stale endpoints.
6. Gate formal execution on a sub-10-second smoke that exercises all four
   Providers and at least one failover for both baselines and one NDNSF request.

## Architecture

### Configuration boundary

Add a campaign profile containing Provider IDs, endpoint/prefix maps, AP
coordinates, coverage range, speed, trace seed, and workload parameters. Keep
the Spec169 default profile unchanged for historical reruns.

### Mobility trace

Generate or replay one trace per `(layout, range, speed, seed)`. Each epoch
records Provider positions, nearest AP, in-range state, and the derived
at-least-one/all-unavailable state. The harness applies the state through its
existing coverage gate, while clients receive no trace or availability file.

### Protocol cells

- NDNSF starts four Provider processes and the normal user/controller path.
- gRPC starts four servers and uses one logical request with explicit
  sequential failover across all targets. `gRPC-SEQ-4` disables proactive
  health routing by default; `gRPC-HC-4` is recorded separately only when the
  explicit application-level health oracle flag is enabled.
- NSC receives four Provider prefixes and advances after Nack/timeout only.

The comparison records a protocol-boundary difference explicitly: NDNSF's
client does not pre-register Provider endpoints; it uses the generic NDN
namespace with runtime forwarding and normal permission/token bootstrap.
The gRPC and NSC clients must be given their four static endpoint/prefix
entries before the workload starts. All four Provider processes still run in
each cell so this registration difference is about discovery/control-plane
setup, not about reducing the available service capacity for a baseline.

All systems share request rate, service delay, deadline, attempt timeout,
traffic barrier, and trace hash. The primary screening matrix is the registered
one-AP range/speed schedule; the harsh single-active handoff schedule remains a
separately labelled stress condition, not an unlabelled post-hoc choice of
range or seed.

### Evidence and claim gate

Each cell writes a manifest, command/source hashes, trace hash, logs, structured
summary, and terminal status. Aggregation operates on paired trace/run units.
The mobility claim is blocked unless SC-005 is satisfied; otherwise the report
must state that no advantage was demonstrated.

## Project Structure

```text
specs/171-four-provider-mobility-advantage/
├── spec.md
├── plan.md
├── research.md
├── data-model.md
├── quickstart.md
├── contracts/experiment-contract.md
├── checklists/requirements.md
└── tasks.md

Experiments/
├── WifiRouterMobilityReliability.py
├── gRPC/greeter_failover_client.py
└── NDN_NSC/consumer.cpp

tests/python/
├── test_mobility_harness_contract.py
├── test_grpc_three_provider_failover.py
└── test_nsc_three_provider_failover_client.py

results/wifi_router_mobility_four_provider_*/
```

**Structure Decision**: Extend the current Spec169 harness and clients in
place, with a new campaign profile and separate result namespace. Do not
rewrite the NDNSF runtime or merge new evidence into the Spec169 directory.

## Validation Gates

1. Static contract gate: Provider cardinality, profile validation, trace schema,
   and no-oracle checks pass.
2. Client gate: gRPC and NSC focused tests pass with four targets/prefixes and
   preserve the three-target compatibility tests.
3. MiniNDN smoke gate: all three systems start, at least one failover occurs,
   and summaries parse in under ten seconds.
4. Paired screening gate: at least two speed/range cells complete with matched
   trace hashes before formal repetitions.
5. Formal evidence gate: all registered cells retain manifests and terminal
   results; failed cells are not automatically rerun.

## Complexity Tracking

No constitutional violations. Four Providers and multiple APs are required to
test the stated mobility hypothesis; a single-AP, three-Provider extension
cannot distinguish Provider diversity from simple endpoint retries.

## Confirmatory gRPC comparison and figure contract

The corrected exploratory 50 m / 2 m/s NDNSF cells used a 2 s trace-relative
measurement start while the retained gRPC/NSC cells used 4 s. Sharing the trace
file is therefore insufficient to establish a paired result, and those mixed
cells must not be used as final publication evidence.

The confirmatory holdout freezes the current one-AP 50 m / 2 m/s configuration,
uses new seeds 43--47, and executes NDNSF, `gRPC-SEQ-4`, and diagnostic
`gRPC-1` cells through the same wrapper with a 4 s measurement start. The
analysis records coverage during the actual 60 s measurement window rather
than over the full trace horizon. It produces a machine-readable summary,
CSV table, and publication SVG/PDF/PNG with source hashes and paired seed
points. The primary interpretation is success non-inferiority plus mean/tail
latency reduction versus sequential retry; `gRPC-1` only demonstrates the
availability cost of a fixed endpoint without failover.

## Seed-variation follow-up

The five-seed holdout is adequate for an honest negative result but has wide
trace-to-trace variation. A bounded follow-up therefore uses ten new independent
seeds (50--59) under the same frozen 50 m / 2 m/s one-AP condition and repeats
three selected seeds in independent processes. Seed-level bootstrap remains the
primary inference; process repeats quantify runtime variation within a fixed
trace and are not counted as new mobility seeds. The protocol and output roots
are registered in
`evidence/followup-seed-repeat-registration-20260807.md`.
The completed analysis is retained in
`evidence/seed-repeat-followup-20260807/`; its ten-seed gRPC interval still
includes zero, and the combined 15-seed interval is
$[-3.73,+1.62]$ percentage points.

## Execution backend policy

This mobility campaign is local-first. MiniNDN-WiFi is the final network
evidence path, so TigerCluster is not required for the registered seed/repeat
follow-up and must not be used for unbounded or purely exploratory reruns.
Agent-selected TigerCluster execution is allowed without an additional user
confirmation only when a future registered cell genuinely requires capacity
unavailable locally. In that case local gates, immutable source/trace/seed
identity, bounded remote preflight, a fresh result root, and at-most-once
submission are mandatory; failed remote identities remain immutable. See
[`docs/tigercluster-execution-policy.md`](../../docs/tigercluster-execution-policy.md).

## Correctness-first Sync recovery gate

The 35-second seed-61 reverse-ACK diagnostic showed that serial Sync production
could complete 175/175 requests, so parallel production was made explicit
opt-in. Independent 60-second replays then falsified the claim that this change
was sufficient: outcomes varied from 132/300 to 192/300 without dense core
TRACE, even though the canonical trace had a reachable Provider for 99.5% of
the measurement epochs. Disabling parallel receive processing improved one
replay to 282/300, while dense core TRACE changed the same serial configuration
to 300/300. This timing sensitivity makes both parallel paths explicit opt-in,
but does not close the defect.

Matrix expansion remains blocked. Recovery is split into two independently
attributable slices. First, preserve the 500 ms ACK window and trace the ACK
sequence through synchronous publication completion, remote state-vector
receipt, mapping fetch, publication fetch, and ACK match. A non-postponed
500 ms periodic retransmission was implemented in an isolated NDN-SVS build but
reduced completion to 116/300, so that hypothesis is rejected and the active
NDN-SVS worktree was restored. Packet capture then established that an
iptables-disconnected Provider continued to receive User traffic after the
gate reopened but its old NFD UDP faces emitted no packets. Recreating the
faces was insufficient until the group and identity routes were explicitly
rebound to the newly returned numeric face IDs. This harness repair completed
the fixed 35-second trace at 175/175 without changing NDNSF selection or
timeouts. It is applied symmetrically to NDNSF and NSC; gRPC does not use NFD.
The repair subsequently passed three independent 60-second fixed-trace
replays. In every run, all 297 requests published with at least one reachable
Provider completed ACK fetch, selection, and Response; only the same three
requests published during zero-Provider coverage timed out. SC-010 is closed,
so the second slice may now evaluate response-level retry/reselection within
the unchanged 5 s global deadline.
The default-disabled implementation has passed both a controlled MiniNDN
mechanism pilot and a fixed-trace mobility integration pilot. The controlled
run reselected one request from deliberately slow Provider A to standby
Provider B after the 1 s attempt timeout and completed 25/25 requests. The
150 m / 10 m/s / seed-61 replay then exposed and repaired a pre-decryption gate
that discarded standby ACKs after immediate FirstResponding selection. With
that repair, all 75 requests completed and all 17 Response-attempt timeouts
were recovered by reselection under the unchanged 5 s global deadline. Three
independent 60-second SC-011 replays then completed 900/900 requests. All 177
requests with a Response-attempt timeout recovered through bounded
reselection, with no timeout after reselection. SC-011 is closed and the
matrix-expansion gate is open.
The parallel environment variables remain diagnostic opt-ins and are not
paper-evidence defaults.
