# T011 MiniNDN Recovery Closure

## Outcome

`PASS`

The functional MiniNDN recovery matrix validates exact-progress resume,
fail-closed expiry and capacity handling, digest/identity isolation, CAS
deduplication, and receipt-defined partial replica durability. It makes no
throughput or latency claim.

## Canonical evidence

- Experiment plan:
  `evidence/us2/t011-experiment-plan.md`
- Canonical run:
  `evidence/us2/minindn-recovery-20260730T034950Z/`
- Aggregate result:
  `evidence/us2/minindn-recovery-20260730T034950Z/summary.json`
- Per-role results:
  `evidence/us2/minindn-recovery-20260730T034950Z/results/`
- Per-process logs:
  `evidence/us2/minindn-recovery-20260730T034950Z/logs/`

The retained Material Passport identifies `experiment-agent`, `run`,
2026-07-30, `VERIFIED`, and version `spec164_t011_recovery_v1`.

## Measured functional observations

| Case | Transferred-byte evidence | Terminal evidence |
|---|---:|---|
| Publisher/repository interruption | 16,384 bytes before interruption; 49,152 bytes after restart | repo1 `ACTIVE`; publisher and repository each used two processes |
| Consumer interruption | 16,384 bytes before interruption; 49,152 bytes after restart | destination atomically visible with matching artifact digest |
| Lease expiry | 0 bytes | expired, later work rejected, no active artifact |
| Changed identity | 0 additional bytes | exact identity conflict rejected |
| Concurrent same digest | 65,536 network bytes for 131,072 logical bytes | second operation had no missing chunks |
| Concurrent different digest | two isolated concurrent operations | no storage-identity cross-contamination |
| Three-replica partial commit | repo3 transferred 0 bytes before capacity rejection | requested 3, achieved 2; repo1/repo2 `ACTIVE`, repo3 `FAILED`; two distinct authenticated receipt IDs |

The canonical matrix completed in 8,452.074 ms. This elapsed value is retained
only as a watchdog/operations observation and is not a performance result.

## Commands and verification

```text
sudo -n -E python3 Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py \
  --recovery-matrix \
  --output-dir specs/164-distributed-repo-large-artifact-transport/evidence/us2/minindn-recovery-20260730T034950Z \
  --payload-size 65536 --timeout-seconds 20
=> PASS, 4/4 cases

build/unit-tests --run_test=DistributedRepoArtifactManifest/*
=> 9/9 passed

build/unit-tests --run_test=DistributedRepoArtifactTransfer/*
=> 8/8 passed

PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
  python3 -m unittest discover -s tests/python -p 'test_spec164*.py'
=> 53/53 passed

PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper \
  python3 -m unittest discover -s tests/python \
  -p 'test_ndnsf_repo_exact_packets.py'
=> 12/12 passed

sudo -n -E python3 Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py \
  --quick-smoke --output-dir /tmp/spec164-t011-base-smoke-20260730T035237Z \
  --timeout-seconds 8
=> SPEC164_ARTIFACT_MININDN_SMOKE_OK
```

The dedicated interruption smoke also passed in 7,581.024 ms, below the
10-second functionality-smoke limit. It used 32 KiB and transferred exactly
16 KiB before and 16 KiB after each restart.

## Implementation findings closed by T011

- Chunk segment coordinates are local to the signed `{chunk}` name component.
  Native verification now explicitly accepts chunk 1 segment 0 and rejects
  global segment coordinates.
- An exact live capacity reservation can be adopted by a restarted repository
  process. Durable authority remains the operation/artifact/generation/lease/
  size/expiry binding; the process owner is replaced only after that binding
  matches exactly.
- Destructive cancellation transitions a nonterminal artifact lifecycle to
  `FAILED`, preventing a discarded temporary object from remaining
  semantically `RECEIVING`.

## Integrity and limitations

- Root/page refetches are bounded recovery metadata traffic; the byte totals
  above count bulk artifact payload only.
- Collaboration authorization and lease issuance are outside this T011 data
  plane matrix and were exercised by T007.
- Evidence sanitation removed the receipt HMAC key, source payload, temporary
  repository stores, consumer destination, and process-control markers.
- Two pre-canonical harness-development runs were discarded: one exposed a
  timestamp-monotonicity error in the harness and one exposed missing
  reservation adoption after process restart. Neither is treated as protocol
  performance or acceptance evidence.
