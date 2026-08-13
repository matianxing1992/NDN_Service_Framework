# Spec 171 Traceability

This matrix separates the registered mobility claim from the confirmed
multi-provider work-efficiency mechanism. A negative SC-005 result is an
accepted outcome under SC-006; it is not silently converted into a positive
mobility claim.

| Requirement | Implementation / contract | Verification evidence |
|---|---|---|
| FR-001 | `configure_profile()` and `provider_nodes()` in `Experiments/WifiRouterMobilityReliability.py` | 4-provider campaign manifests for seeds 20--25 |
| FR-002 | campaign configuration and shared trace hash validation | 18 paired cells; `fairness-audit-20260806.md` |
| FR-003 | strict sequential gRPC client and four-target NSC wiring | `test_grpc_three_provider_failover.py`; `test_nsc_three_provider_failover_client.py` |
| FR-003a | `--grpc-no-health-routing` and separate health-routing fields | six-seed gRPC manifests and summaries |
| FR-004 | NDNSF `first-responding` strategy and provider execution counters | NDNSF per-provider execution counts; work-efficiency aggregates |
| FR-005 | AP layout/range arguments and trace metadata | random-waypoint manifests; coverage fractions in retained aggregates |
| FR-006 | speed profile validation and manifest recording | `test_four_provider_mobility_profile.py`; seeds 20--25 at 8 m/s |
| FR-006a | registered `single-active-handoff` generator and contract checks | `test_mobility_harness_contract.py`; formal single-active evidence |
| FR-007 | coverage-gated label and no-oracle trace plumbing | campaign manifests and `fairness-audit-20260806.md` |
| FR-008 | stable logical request IDs, bounded attempts, terminal summaries | all 18 passed cell summaries |
| FR-009 | trace-derived availability and all-unreachable counters | per-seed trace validation; all-unreachable count 0 |
| FR-010 | smoke-mode validation path | mobility harness contract suite |
| FR-011 | 60-second window, warmup/barrier, lateness gate, no automatic rerun | six-seed manifests; lateness max 8.8 ms |
| FR-012 | unique campaign directories, manifests, hashes, summaries, aggregates | retained pilot and confirmatory result directories |
| FR-013 | separate Spec169/171 result namespaces | historical-result boundary in fairness audit |
| FR-014 | claim gate blocks unsupported mobility advantage | SC-005/SC-006 verdict in fairness audit and paper text |
| FR-015 | paired timeout override registration and explicit FirstResponding semantics | `registration.json`; `timeout-sensitivity-2s5s-50m2ms-3seed-20260806.md` |
| FR-016 | independent trace-paired holdout with lateness rejection | `holdout-confirmation-result-20260806.md`; holdout manifests |
| FR-017 | publication metrics, paired confidence bounds, and input hashes | `holdout-comparison-20260806/`; publication figure inputs |
| FR-018 | parallel Sync receive/production are explicit opt-ins with runtime observability | `test_spec171_parallel_svs_production.py`; corrected User/Provider logs |
| FR-019 | independent 60-second replay and ACK/Response failure classification | `ndnsf-100m-10ms-seed61-ack-path-diagnosis-20260808.md`; full-window lifecycle CSVs |
| FR-020 | ACK publication/state-vector/mapping/publication-fetch observability and evidence-supported recovery | numeric NFD face-ID/FIB reconnect repair; three independent 60-second replays each delivered 297/297 reachable-at-publication requests |
| FR-021 | bounded post-selection Response retry/reselection | default-disabled implementation; 25/25 controlled mechanism pilot; repaired 150 m fixed-trace pilot delivered 75/75; three 60-second replays delivered 900/900 with 177/177 timed attempts recovered |
| SC-001 | four-provider smoke/summary parser | focused mobility/failover tests |
| SC-002 | paired formal cells across coverage/speed conditions | formal and supplementary evidence directories |
| SC-003 | 300 requests at 5 RPS or terminal setup failure | 18 work-efficiency cells each sent 300 |
| SC-004 | success, retries, latency, provider work accounting | per-cell summaries and aggregate CSV/JSON |
| SC-005 | registered single-active-handoff lower-bound gate | **Not met**; no demonstrated mobility advantage |
| SC-006 | explicit negative mobility verdict and preserved evidence | `fairness-audit-20260806.md` and paper/slide claim boundary |
| SC-007 | paired 2 s/5 s timeout sensitivity and lifecycle-stage accounting | `timeout-sensitivity-2s5s-50m2ms-3seed-20260806.md`; `registration.json` |
| SC-008 | five-seed holdout and conditional claim gate | `holdout-confirmation-result-20260806.md`; `NO_HOLDOUT_CONFIRMATION` |
| SC-009 | fixed seed-61 corrected-default replay reaches every lifecycle stage | `ndnsf-100m-10ms-seed61-ack-path-diagnosis-20260808.md`; 175/175 corrected-default summary |
| SC-010 | three independent full-window ACK-path recovery replays | **PASS**; all three runs delivered ACK, selection, and Response for 297/297 reachable-at-publication requests; only the same three zero-coverage requests timed out per run |
| SC-011 | three independent Response retry/reselection replays | **PASS**; Runs 2/3/4 replayed one canonical trace in fresh MiniNDN processes, delivered 900/900, and recovered all 177 requests with a Response-attempt timeout through 179 bounded reselections |

## Confirmed supplementary mechanism endpoint

The capacity-matched six-seed repeat is not a replacement for SC-005. It is a
separate, reviewer-facing endpoint for the multi-provider API mechanism:

- NDNSF: 1,798/1,800 success, 1,800 provider executions.
- gRPC-PAR-4: 1,800/1,800 success, 7,180 server executions, 5,380 extra.
- NSC-4: 1,800/1,800 success, 1,804 sequential attempts.
- Same trace per seed, `block_network=true`, four workers, 250 ms service,
  six seeds, all-four coverage at least 0.9586, and no all-unreachable epoch.

The machine-readable pooled evidence is
`results/four_provider_work_efficiency_confirmatory_20260806/combined-six-seed-aggregate.json`.
