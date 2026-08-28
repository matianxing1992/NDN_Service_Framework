# T009 Provider Cache and Residency Evidence

**Status**: implementation-complete local contract evidence; TigerCluster CUDA
acceptance remains pending under T011.

## Implemented boundary

- `ProviderResidencyIdentity` is a canonical NDNSF-DI Core contract. Durable
  compatibility binds model content, dependency graph, partition candidate,
  artifact bytes, adapter identity/version, and backend. RAM is additionally
  Provider-boot-bound; GPU is boot- and exact-device-bound.
- `ProviderResidencyLedger` owns one content-addressed disk path per artifact,
  link-only request views, DISK/RAM/GPU promotion, owner-protected eviction,
  restart invalidation, and cumulative byte/load/hit counters.
- The Qwen Selection V2 Provider fetches directly into the content-addressed
  path, promotes to GPU only after exact-device load and successful warmup with
  zero CPU fallback, and releases the request owner at transaction terminal
  without evicting the reusable model.
- ACK generation reads the ledger. `DIProviderOfferIssuer` returns the negative
  decision `DI_RESIDENCY_EVIDENCE_INVALID` instead of signing an incomplete,
  stale, wrong-boot, wrong-backend, or wrong-device cache claim.
- `PreSplitFirstStrategy` now requires the advertised `partition_digest` to
  equal the selected candidate digest. Matching only model and graph identity
  is insufficient.

The frozen Spec 162 job source was not rewritten. A new Spec 168 remote job
must populate `partition_digest`, `adapter_id`, and `adapter_version` in each
residency template before a candidate SIF can be admitted.

## Focused verification

Run from the repository root with:

```bash
PYTHONPATH=.:NDNSF-DistributedInference:NDNSF-DistributedRepo/pythonWrapper \
  python3 -m unittest discover -s tests/python -p 'test_spec168_*.py'
PYTHONPATH=.:NDNSF-DistributedInference:NDNSF-DistributedRepo/pythonWrapper \
  python3 tests/python/test_ndnsf_di_presplit_first_strategy.py
PYTHONPATH=.:NDNSF-DistributedInference:NDNSF-DistributedRepo/pythonWrapper \
  python3 tests/python/test_ndnsf_di_selection_dataflow.py
PYTHONPATH=.:NDNSF-DistributedInference:NDNSF-DistributedRepo/pythonWrapper \
  python3 tests/python/test_spec162_qwen36_repo_prepare.py
```

Result on 2026-08-03: `35 + 8 + 22 + 1 = 66` tests passed.

The two-request Qwen preparation regression produced this causal counter
sequence from one unchanged Provider process:

| Counter | After cold request | After compatible warm request |
|---|---:|---:|
| `repoUniqueBytes` | 18 | 18 |
| `repoWireBytes` | 18 | 18 |
| `deviceLoadCount` | 1 | 1 |
| `gpuHitCount` | 1 | 2 |
| `duplicatePayloadBytes` | 0 | 0 |

It also proved `fetch_file`, model load, and CUDA warmup were each called once
across two requests; both request owners were released and the GPU record
remained resident. Separate tests prove symlink views, DISK-to-RAM-to-GPU
promotion, owner-protected eviction, boot/device fallback, and model, graph,
partition, artifact, adapter-version, and backend mismatch rejection.

## Claim boundary

The focused tests use temporary files and a CUDA-shaped adapter fixture. They
prove state-machine, identity, ownership, counter, and Provider-integration
logic; they do **not** prove real CUDA allocation, real GPU memory retention, or
TigerCluster latency. Those claims require the unchanged-allocation T011
campaign and its retained ACK/counter/CUDA evidence.
