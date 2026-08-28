# TigerCluster Gate E job 182411 — incomplete residency identity

Status: **FAILED (preserved negative evidence; never retry this campaign)**

- Campaign: `spec168-campaign-v3-cc950f1684c07ea491d9`
- Request: `/spec168-cc950f1684c07ea491d9-single`
- Source identity: `sha256:1e192ca033600a6c4b10ad1e2393172989d08ff129956aaf551712d5e06b1949`
- Slurm: `FAILED 1:0`, elapsed 5:55 on three RTX 5000 nodes

## Result and root cause

The run passed NFD/full-mesh routes, controller policy/identity bootstrap, all
three DistributedRepo startups, automatic planning, all three Provider
startups, and publication/decryption of one real Request. Every Provider's
Python ACK callback raised the same bounded error:

```text
ValueError: partition_digest must be a canonical sha256 digest
```

The v38 observability repair worked: each exception became a fail-closed
`INTERNAL_ERROR` negative ACK, the requester closed with `ackCount=3`, and the
failure was no longer misreported as zero ACK/network loss. No model fetch,
CUDA inference, or data-dependency execution began.

The deployment launcher generated each `selection-residency-*.json` without
the required `partition_digest`, `adapter_id`, and `adapter_version` fields,
despite all three values already existing in `automatic-planning.json`. The
Provider therefore could not construct the durable content-addressed residency
identity used by ACK cache evidence.

The rank-abort path closed the three-node job promptly. Automatic cleanup
removed one ephemeral selection-key file and retained no secret key/token files
(`raw/security-scrub.txt`).

## Remediation boundary

The next source identity must copy `candidateDigest`, `adapterId`, and
`adapterVersion` into every residency record and validate the complete identity
before declaring `LLM_PIPELINE_PROVIDER_READY`. This converts future malformed
deployment metadata into a Provider-startup failure, before a user Request.
