# Four-Provider Mobility Screening Evidence

This is screening evidence, not a paper claim. All three systems used the
same replayed trace within each campaign. The MiniNDN cells were run as root
because Mininet-WiFi requires network namespaces.

## Paired smoke

Command:

```text
sudo -n python3 Experiments/WifiRouterMobilityReliability.py \
  --profile four-provider-multi-ap --smoke --include-ndnsf \
  --lock-file /tmp/ndnsf-four-provider-paired.lock \
  --output-dir results/four_provider_mobility_paired_smoke_20260805
```

Trace: 200 m coverage, 8 m/s, four Providers, deterministic forced outage of
UCLA at 2.4--3.4 s, 25 logical requests per system.

| system | success | attempts | failovers | result |
|---|---:|---:|---:|---|
| NDNSF | 25/25 (100%) | n/a | n/a | smoke pass |
| gRPC-HC-4 | 25/25 (100%) | 26 | 1 | smoke pass |
| NSC-4 | 25/25 (100%) | 26 | 1 | smoke pass |

Evidence directory: `results/four_provider_mobility_paired_smoke_20260805/`.

## Harsh screening

Command profile: 75 m coverage, 15 m/s, 5 s, 5 RPS, 500 ms logical deadline,
150 ms baseline attempt timeout, 200 ms health interval. The trace hash is
`4647a1a5cbfe796f537a3a2a1a047f03f5680f6f9c8de04e459737c0c4fae923`.

| system | success | attempts | failovers | p95 (ms) |
|---|---:|---:|---:|---:|
| NDNSF | 24/25 (96%) | n/a | n/a | not emitted by current user marker |
| gRPC-HC-4 | 25/25 (100%) | 28 | 3 | 170.652 |
| NSC-4 | 24/25 (96%) | 45 | 20 | 474.323 |

Evidence directory: `results/four_provider_mobility_harsh_75m_15mps_20260805/`.

## Stale-health stress

Replayed the harsh trace with a 300 ms logical deadline, 100 ms attempt
timeout, and 1 s health interval.

| system | success | interpretation |
|---|---:|---|
| NDNSF | 25/25 (100%) | parallel ACK/selection path survived this trace |
| gRPC-HC-4 | 24/25 (96%) | one logical deadline failure after stale health |
| NSC-4 | 22/25 (88%) | three terminal failures and 17 failovers |

Evidence directory: `results/four_provider_mobility_stalehealth_300ms_20260805/`.

## Claim decision

The smoke confirms the four-Provider harness, authentication, trace replay,
and sequential retry accounting. The stale-health screen is a promising
direction, but one 25-request run has no confidence bound and cannot satisfy
SC-005. Formal repetitions remain justified only for the registered
300 ms/100 ms/1 s stress cell (and its matched seeds); do not update slide 32
or the paper table from these screening numbers.
