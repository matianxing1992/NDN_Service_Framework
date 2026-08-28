# Provider-transition experiment registration

## Claim under test

NDNSF can use a newly reachable, already authorized and routed Provider without
placing Provider identities or addresses in the user command. Static gRPC and
NSC clients cannot use an omitted Provider; a pre-registered four-endpoint
control demonstrates that the service capacity itself is available.

This does **not** claim that gRPC or NSC cannot implement dynamic discovery.
They can use an external resolver or configuration-update control plane. The
experiment measures whether that additional application dependency is present.

## Frozen transition

- Provider order: UCLA/A, WUSTL/B, UIUC/C, Arizona/D.
- Trace time 0--20 s: A--C reachable, D unreachable.
- Trace time 20--40 s: A--D reachable.
- Trace time 40--70 s: only D reachable.
- Measurement: trace time 4--64 s, 5 RPS, 300 requests.
- Network enforcement: `block_network=true` with NFD face repair.
- Timeouts: 1 s attempt/ACK, 5 s global.
- NDNSF: FirstResponding with bounded Response reselection; admission disabled.
- gRPC: sequential attempts, health routing disabled.
- Trace SHA-256:
  `fec7cec0f8e0e5a2f01d405ff53d88303d474463a4c3a450b4deefcd9e05f7ed`.

## Registered cells

| Cell | Client configuration | Purpose |
|---|---|---|
| NDNSF | `/HELLO`; zero Provider identities/addresses | internal service discovery |
| gRPC-static-3 | A--C addresses | omitted-Provider control |
| gRPC-preregistered-4 | A--D addresses | capacity/preregistration control |
| NSC-static-3 | A--C prefixes | omitted-Provider control |
| NSC-preregistered-4 | A--D prefixes | capacity/preregistration control |

The pilot is one complete 60-second replay of all five cells. Only if runtime
commands, trace hashes, 300-request counts, and per-window result accounting
pass will two more independent process replays be run. A failed or negative
cell is retained and is not automatically replaced.

## Acceptance

For each of three complete replays:

1. NDNSF's retained user command contains zero Provider identities and zero IP
   addresses.
2. NDNSF completes at least 95% of requests published at trace time 40--64 s,
   and Provider D publishes those Responses.
3. Static three-target controls execute zero requests at D and cannot complete
   requests after A--C retire.
4. Pre-registered four-target controls complete at least 95% of the same
   post-retirement window.
5. Runtime commands, trace hash, result rows, attempts/executions, and latency
   are retained. Negative outcomes narrow the claim rather than being rerun.
