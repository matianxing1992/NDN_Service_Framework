# Job 182512: control-plane canary incorrectly entered model preflight

- Candidate: `20260804T155933Z-v84-control-plane-canary`
- Source identity: `sha256:2913c4bc8954366561a2b5f42a3f148f038c8a756d2ed717e5d3a2461d88628a`
- Source bundle: `sha256:60954ef4c6d7c012f50701fa3eca0d1b60c20601d0946a49d66f9707e120a3ad`
- Campaign: `spec168-campaign-v3-d734f5beebebfb466360`
- Slurm job: `182512`, one admitted submission, no retry
- Slurm result: `FAILED`, elapsed `00:01:34`, exit `1:0`

## Narrowest failure boundary

The job failed before `srun`, NFD startup, Request publication, ACK collection,
Selection delivery, Provider execution, model publication, or CUDA work.  The
single-node batch wrapper reused the normal model preflight even though
`SPEC168_CONTROL_PLANE_CANARY=1`: it hashed the Qwen artifact directory and
required the generation template to contain exactly one prompt.  The reused
formal template contains five prompts, so the terminal error was
`SPEC168_SINGLE_CONTROL_PROMPT_REQUIRED`.

This is a locally detectable harness/admission defect, not a TigerCluster,
NDNSF, DistributedRepo, model, or GPU failure.  The 94-second runtime was spent
before protocol execution, including unnecessary model-artifact checksum work.

## Repair and prevention

The canary path is separated from model mode before artifact validation.  It
requires only the deployment policy plus the real SIF/source/campaign/security
bindings; it may not accept a stage manifest or generation template, start
DistributedRepo nodes, build automatic planning, or pass model identity and
workload digests.  `spec168-prepare-single-campaign.py` provides an executable
local regression that succeeds without any model input and rejects model paths
in canary mode.  The formal model path retains its prior checks.

Job 182512 and its raw evidence remain immutable.  Any requalification must use
a new source identity and campaign; the failed campaign is never resubmitted.

