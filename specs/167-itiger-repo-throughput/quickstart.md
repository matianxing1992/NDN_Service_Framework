# Quickstart: Spec 167

No formal TigerCluster job may be submitted until the local and remote
preflights pass under the same immutable source identity.

The Qwen3.6-27B TigerCluster attempts recorded in Spec 162 are diagnostic
context only. They published 53.79 GB of model stages through DistributedRepo,
but their elapsed time includes model staging, registration/catalog commit,
provider setup, and inference control traffic. Do not use those jobs as a
Spec 167 throughput cell. Spec 167 must retain controlled payload sizes,
matched node pairs, cold publication, cold fresh-destination retrieval, warm
content-addressed reuse, and a separately measured physical-network ceiling.

```bash
# Local contract and analyzer gates
python3 -m unittest \
  tests.python.test_spec167_itiger_artifact_runner \
  tests.python.test_spec167_itiger_job_contract

# Exact candidate-container gate (the source-root mount is the staged bundle)
SPEC167_SOURCE_ROOT="$PWD/results/spec167-source-<id>" \
  specs/167-itiger-repo-throughput/jobs/validate-local-candidate.sh

# Stage immutable source and run the bounded remote preflight
rsync -a --checksum --delete-delay \
  specs/167-itiger-repo-throughput/jobs/ \
  itiger:/project/tma1/ndnsf-di/jobs/spec167/source-001/
ssh itiger 'cd /project/tma1/ndnsf-di/jobs/spec167/source-001 && sha256sum -c source-checksums.sha256'
ssh itiger 'sbatch --comment=spec167:spec167-repo-preflight-001 repo-throughput-preflight.sbatch'
```

The formal submission command is frozen only after the remote preflight result
is recorded.  Never reuse a failed submission ID or overwrite its `.partial`
evidence directory.

Before staging, the candidate source root must contain
`Experiments/spec164_artifact_campaign.py` and its `source-manifest.json` must
list and hash that file. The local gate runs
`Experiments/validate_spec167_source_bundle.py` when a manifest is present;
an omitted runtime helper fails locally before Docker or Slurm is invoked.

## Closed source-013 campaign

The first complete TigerCluster result is frozen under source-013. Do not
submit it again or rebuild the SIF:

- preflight: `spec167-repo-preflight-013` / job `181821` (`PASS`);
- formal campaign: `spec167-tiger-20260802-source013` / job `181822` (`PASS`);
- analyzer: `SPEC167_ANALYSIS_PASS`, 60/60 rows, 10 warmups, 50 measured;
- remote evidence: `/project/tma1/ndnsf-di/evidence/spec167/campaign/spec167-tiger-20260802-source013`;
- local mirror: `results/spec167-source-013-freeze/remote-evidence/`.

The source-012 failure remains immutable negative packaging evidence. Source-013
adds the existing `Experiments/spec164_artifact_campaign.py` runtime helper;
it does not change the payload, schedule, SIF, or transport protocol.
