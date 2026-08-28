# Provider discovery and conditional switching evidence

## Exact supported claim

NDNSF's client is configured with a service name and no Provider identity or
address list. For a Provider that is already authorized and routed, NDNSF can
discover the Provider through the normal request/ACK exchange. In windows where
the sequential baseline's first endpoint is unreachable but another Provider
is reachable, this mechanism can reduce the tail cost of endpoint discovery
and sequential retry.

This is not a claim that gRPC or NSC cannot support dynamic discovery. They can
do so through an external resolver, service registry, or configuration-update
control plane; those mechanisms are additional configuration and are outside
the evaluated static controls.

## Panel A: no client-side Provider pre-registration

The deterministic transition has three phases: Providers A--C are initially
reachable, all four overlap briefly, then only Provider D remains reachable.
NDNSF receives only `/HELLO`; the static controls receive A--C; the capacity
controls receive A--D before measurement. gRPC application-health and resolver
routing are disabled. Every cell runs 300 requests at 5 RPS for 60 seconds,
with 1-second attempt/ACK timeouts and a 5-second global deadline. Admission
control is disabled.

Across three independent process replays, the steady post-retirement window
contains 357 requests per system. The request scheduled exactly at each of the
20 s and 40 s trace transitions is retained under `transition_boundary`, not
assigned to either adjacent steady state, because the four provider gates are
applied sequentially rather than atomically:

| System | Client Provider endpoints | Success | Provider D successes | Median replay p95 |
|---|---:|---:|---:|---:|
| NDNSF | 0 | 357/357 | 357 | 107.08 ms |
| gRPC static 3 | 3 | 0/357 | 0 | n/a |
| gRPC pre-registered 4 | 4 | 357/357 | 357 | 3030.02 ms |
| NSC static 3 | 3 | 0/357 | 0 | n/a |
| NSC pre-registered 4 | 4 | 357/357 | 357 | 3047.00 ms |

The static-control failures show that an omitted endpoint cannot be retried.
The pre-registered controls show that Provider D had usable service capacity.
NDNSF's 357 Provider-D completions with zero client endpoints are therefore the
direct evidence for service-name-based Provider discovery.

## Panel B: conditional cost in natural mobility

The frozen 100 m random-waypoint campaign is classified using only the shared
trace after execution. `SWITCH_REQUIRED` means that the deterministic gRPC
first endpoint was unreachable at request publication while at least one later
endpoint was reachable. Across ten paired mobility seeds, 1,747 requests met
that condition. Both NDNSF and gRPC completed 1,745 of them, so this subset does
not support a success-rate advantage.

For each seed, the registered p95 stage metric compares NDNSF request-ID time
to selected Provider `SELECTION_RECEIVED` against the sum of gRPC failed-attempt
durations before the successful RPC. The mean paired reduction is 512.93 ms;
the fixed-seed 20,000-replicate bootstrap 95% interval is
140.50--886.27 ms. Some gRPC seeds fast-fail in about 1 ms and are faster than
NDNSF, so the conclusion is explicitly conditional rather than universal.
These are pre-execution lifecycle-stage metrics, not end-to-end latency; the
frozen NDNSF logs do not contain a user-observed per-request latency for this
comparison.

## Unconditional controls retained

The primary random-waypoint results remain visible: at 50 m, ten-seed success
was 54.57% NDNSF, 53.80% gRPC, and 55.47% NSC, with no NDNSF success or latency
advantage. At 100 m, NDNSF and gRPC both achieved 97.97% success and NSC 98.33%;
NDNSF's advantage there was lower tail latency, not higher success. These
controls prevent the conditional Provider-discovery result from being presented
as a universal mobility advantage.

## Artifacts

- `provider-discovery-and-switching.png`: Google Slides-safe raster figure.
- `provider-discovery-and-switching.pdf`: publication vector output.
- `provider-discovery-and-switching.svg`: editable vector output.
- `figure-data.json`: exact values consumed by the plotting script.
