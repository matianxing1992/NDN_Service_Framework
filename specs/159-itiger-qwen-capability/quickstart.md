# Quickstart

This file records the ordered operator path; exact generated identities and
commands are written to the evidence directory before submission.

1. Verify VPN/SSH, Slurm/GRES, Apptainer, project storage, model revision, and
   local candidate identity.
2. Publish one unique capability tag and record its immutable OCI digest.
3. Render and inspect a short CPU OCI-to-SIF job, then submit it exactly once.
4. After SIF PASS, render and inspect a one-GPU standalone Qwen job, then submit
   it exactly once.
5. After standalone PASS, render and inspect a one-GPU NDNSF-DI Qwen job, then
   submit it exactly once.
6. Copy durable manifests locally and audit every success criterion.

Do not run conversion, inference, NFD, or NDNSF processes on the login node.
