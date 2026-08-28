# Spec 165/166/167 Failure and Retention Audit — 2026-08-01

## Material Passport

- Origin skill: Academic Research Suite experiment-agent, validate mode
- Verification status: ANALYZED for historical remote failures; VERIFIED for
  the local retention implementation and focused tests
- Evidence boundary: no MiniNDN model inference, Docker build, SIF
  materialization, or TigerCluster job was repeated for this audit

## Verdict

**CONDITIONAL PASS.** The accepted inference path is real: Spec 166 job
`181096` completed on three RTX 5000 nodes with 8/8 exact generations, 192/192
CUDA Stage records, complete request-ID lineage, dependency digests, and no CPU
fallback. The current-source Spec 165 A-D aggregate also passes locally. The
remaining block is experiment orchestration, not proof that DistributedRepo
cannot transfer data: Spec 167 preflight `181106` measured 335.06 Mbps forward
and 312.18 Mbps reverse, while formal campaign evidence is incomplete after
jobs `181112` and `181115` exposed two independent runner races.

## Why the failures happened

| Evidence | Root cause | Should a local gate catch it? | Current repair/reuse decision |
|---|---|---|---|
| Spec 165 current-source first aggregate | CUDA ONNX Runtime wheel was unusably slow under declared CPU fallback; container topology used a host-only absolute path | Yes; the corrected strict MiniNDN/container gate did catch both | Reuse the same workload and stage bundle; use the CPU overlay locally and preserve CUDA for GPU-only qualification |
| 181086 | rank entrypoint not executable | Yes, package/executable contract | Fixed by executable-bit test and pre-`srun` check; do not repeat model preparation |
| 181088 | host wrapper referenced container-only `/source/nfd.conf.in` | Yes, host/container path contract | Fixed by source-root regression; reuse SIF and workload |
| 181089 | sealed SIF API lacked `request_id` | Yes, exact candidate import/signature probe | Fixed by checksum-bound API overlay; request ID remains unchanged end-to-end |
| 181091 | overlay omitted `compatibility/manifest.json` | Yes, package-closure probe | Fixed by import/manifest preflight; reuse native parent layers |
| 181092 | bootstrap token was checked before EXIT cleanup | Yes, lifecycle ordering test | Fixed by deleting before `user-done`/rank completion |
| 181094 | timing evidence omitted CUDA, digest, and Data-name fields | Yes, schema validator | Fixed by mandatory structured fields; no inference semantic change |
| 181095 | Python/native stdout writes interleaved | Mostly; needs concurrent mixed-language emission | Fixed by one-syscall atomic markers |
| 181097 | accepted SIF lacked `iperf3` | Yes, exact-image capability probe | Replaced external package assumption with checksum-bound Python TCP ceiling; no image rebuild |
| 181098 | pinned NFD rejected `status` privilege | Yes, start rendered NFD config locally | Fixed template and added exact-candidate NFD-liveness gate |
| 181100 | pre-created coordination directory conflicted with inherited fixture | Yes, exact directory-shape test | Fixed only the portable rank-local path; legacy MiniNDN remains fail-closed |
| 181103 | consumer fetched before producer prefix readiness | Yes, asymmetric two-container/two-NFD test | Added exact producer-ready barrier |
| 181107 | `/project` NFS attribute caching added 30–60 s to file barriers | Cluster-specific timing, but path ownership is statically testable | Data stays rank-local; control metadata moves over node LAN; `/project` is promotion-only |
| 181112 | both ranks attempted TCP face creation before peer listener readiness | Yes, asymmetric-start fault injection | Bounded 2 s attempts within a 30 s face budget; failed identity retained |
| 181115 | `srun` inherited and consumed `schedule.tsv` stdin | Yes, shell job-contract test | Every `srun` now reads `/dev/null`; source-010 evidence remains immutable and incomplete |

The repeated remote failures were therefore caused by missing deployment-
fidelity gates and runner races. They are not evidence of seven independent
NDNSF-DI algorithm failures, nor evidence that 600-second waits were required
for a sub-gigabyte model.

## Work that must not be repeated

- Model identity and workload remain the accepted Qwen3-0.6B snapshot and
  workload digest already recorded by Spec 165.
- The six stage files, 6,014,071,792 bytes total, are installed once at
  `results/_artifacts/qwen-stage-bundles/sha256/ab0695659d5d0a589d89349737d214d2d06b1c3697e013d4409a6a376c41358e`.
- Import used hard links, wrote zero duplicate payload bytes, and replaced the
  originating run's stage directory with a relative link. Twenty-two existing
  references across thirteen runs now point directly to the shared bundle.
- Existing Docker foundations and CUDA/native parent layers are reusable.
  A source/API change may require a thin app/native overlay, but does not
  justify rebuilding the foundation, downloading the model, or exporting the
  ONNX stages again.
- Spec 167 source-010 already proves one complete signed-manifest warmup: 37
  forward/reverse subtransfers, 323.16 Mbps forward, 63.46 s reverse cold
  transfer, zero timeouts/retransmissions, and zero warm duplicate bytes. It is
  incomplete evidence, not a distribution estimate, and must not be relabeled
  or overwritten.

## Minimal repair path

1. Keep the content-addressed stage bundle and current local CPU image; do not
   rebuild or re-export them.
2. Run only fast source/package/job-contract tests for the `/dev/null`, bounded
   face retry, path isolation, manifest, and retention invariants.
3. Freeze a new Spec 167 source identity whose delta from source-010 is the
   stdin fix plus its tests. Reuse the accepted SIF and immutable schedule.
4. Submit one new formal campaign identity. Do not rerun jobs 181112/181115 or
   their completed warmup rows.
5. Analyze only after all preregistered rows exist; keep cold publication,
   cold retrieval, and warm zero-copy reuse separate.

## Local verification

- `python3 -m unittest discover -s tests/python -p 'test_spec165_gate_runner.py'`: 8/8 PASS
- `python3 -m unittest discover -s tests/python -p 'test_spec165_*.py'`: 30/30 PASS
- Actual bundle migration: 6 files, 6,014,071,792 bytes, zero duplicate payload
  bytes, bundle digest `sha256:ab0695659d5d0a589d89349737d214d2d06b1c3697e013d4409a6a376c41358e`

No claim in this document upgrades a preflight rate or one warmup to a formal
throughput distribution.
