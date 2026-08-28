# Canonical Spec 164 MiniNDN Campaign

The active canonical confirmatory campaign is:

```text
results/spec164-artifact-confirmatory-campaign-20260730T1030Z
```

Campaign identity and immutable seals:

```text
campaignId: spec164-artifact-20260730T101414Z
manifestSha256: 0fbcd3d4f77dbadba5e90eaad3a23d8425f9a67e0fbd9fca401cc38aff568996
campaignRunsJsonlSha256: 227cb1fa5cf25d2cb5e3dade751c8548a7f2bfd9018f6a5b85e546b5ce363514
campaignRunsCsvSha256: 72280755af3b9ee39d0cff64955f343aafcdb602043f2354fbdb50d0201fa001
derivedResultsSha256: bd2f011acb959a29bdd250acc65d7819f00eb69ce45a64bf9bb909dcaa50c687
performanceReportSha256: 97c009b99598a73b989779bb64c079614b3408bc114bbebeafea95e0d5540116
```

The preflight evaluated all 96 candidate cells before formal outcomes. It
admitted 24 and retained 72 exclusions with mechanical reason codes. The
frozen schedule and retained outcomes are:

```text
warmup: 24 total, 24 PASS, 0 FAIL
measured: 120 total, 120 PASS, 0 FAIL
measured repetitions per admitted cell: 5
missing or duplicate run IDs: 0
```

The determinate threshold verdicts are:

```text
SC-002: PASS
SC-003: PASS
SC-004: PASS
SC-007: PASS
SC-011: PASS
SC-012: PASS
independent verification: PASS
```

For the predeclared large-artifact domain (64 MiB and above):

```text
SC-002 digest/raw median: 0.975401
SC-002 bootstrap 95% CI: [0.925489, 1.015141]
SC-003 signed/digest median: 0.996644
SC-003 bootstrap 95% CI: [0.984451, 1.069018]
```

All 1 MiB diagnostic cells also remain present and passed, but they do not
define SC-003's large-artifact throughput gate.

SC-004 uses the separate public Collaboration smoke:

```text
results/spec164-public-task-minindn-20260730T0734Z/summary.json
sha256: 22e48024507c9a68dc423365f1357edefa95d21132ec679971da54abcb57ae01
control operations: 2
publication Data segments: 16
selected replicas: 1
lifecycle phases: 3
```

This establishes bounded Request/Selection control for the observed
publication and no per-segment NDNSF service invocation. The data-plane
campaign independently measures publication, verification, persistence, and
cold retrieval. These latency samples are not combined.

Re-run the analyzer without modifying the frozen ledger:

```bash
python3 Experiments/analyze_distributed_repo_artifact.py \
  --campaign results/spec164-artifact-confirmatory-campaign-20260730T1030Z \
  --output-json \
    results/spec164-artifact-confirmatory-campaign-20260730T1030Z/derived-results.json \
  --output-markdown \
    specs/164-distributed-repo-large-artifact-transport/evidence/remediation/t031-performance-report.md \
  --control-evidence \
    results/spec164-public-task-minindn-20260730T0734Z/summary.json
```

All predecessor campaigns remain immutable, including the third campaign's
three measured first-Interest failures and original SC-003 `FAIL`:

```text
results/spec164-artifact-stability-campaign-20260730T0935Z
results/spec164-artifact-remediation-campaign-20260730T0820Z
results/spec164-artifact-campaign-20260730T050211Z
```

The confirmatory campaign did not repeat the physical ceiling. The
predecessor's 659.613 Mbit/s ceiling is historical environment evidence, not
a property silently assigned to this run.

Spec 164 now permits TigerCluster as external-validity work for NDNSF-DI and
Qwen, subject to Specs 162/163's own frozen configuration, security,
correctness, resource, and evidence gates. No MiniNDN result is itself a
TigerCluster or large-model result.
