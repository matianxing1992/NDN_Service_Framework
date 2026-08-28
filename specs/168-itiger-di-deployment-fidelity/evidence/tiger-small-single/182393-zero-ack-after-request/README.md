# TigerCluster Gate E job 182393 — zero ACK after real Request

Status: **FAILED (preserved negative evidence; never retry this campaign)**

- Campaign: `spec168-campaign-v3-ff47a90a70db779277be`
- Request: `/spec168-ff47a90a70db779277be-single`
- Source identity: `sha256:315c94fd0c75f8491c19e7f01f0811620a4992c9544f185bdec38bd5e6f2a6f4`
- Slurm job: `182393`, final state `FAILED 1:0`
- Runtime: three RTX 5000 nodes using the existing sealed Spec 167 SIF and existing Qwen3-0.6B artifact

## Observed boundary

The job passed NFD/route startup, seven-identity controller bootstrap, three
DistributedRepo startups, automatic graph planning, three Provider startups,
and real request publication without a fixed Provider-settle delay. Every
Provider decrypted and dispatched the same V2 request. The requester then
closed its 120-second ACK window with `ackCount=0`; no model preparation or
inference began.

This failure is therefore in the Provider ACK control path, before model fetch,
CUDA execution, or data-dependency execution. Inspection of the Python/native
adapter showed that an exception escaping a Python ACK handler is converted by
the native binding into a suppressed ACK without a provider-side diagnostic.
That behavior is consistent with all retained observations, but the exact
Python exception was not observable in this frozen run.

The v37 rank-abort repair worked: rank 0 published the abort marker and the
other ranks exited instead of waiting for the 18,000-second outer timeout.
Automatic cleanup removed one ephemeral selection-key file and retained no
secret key or token files (`raw/security-scrub.txt`).

## Evidence

- `raw/node-0/user.log`: Request sent, ACK closure, terminal response failure.
- `raw/node-{0,1,2}/provider-*.log`: decrypt/permission/dispatch on all Providers.
- `raw/rank-abort`, `raw/rank-failed-0.txt`, `raw/outer-rank-failed-0.txt`: bounded cross-rank abort.
- `raw/security-scrub.txt`: post-run secret cleanup.
- `slurm-182393.out`, `slurm-182393.err`: scheduler-level output.

The next source identity must make Python ACK exceptions visible as a bounded,
sanitized provider diagnostic and a fail-closed `INTERNAL_ERROR` negative ACK.
It must not retry or overwrite this campaign.
