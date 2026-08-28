# Gate E job 182389 - Repo identity policy mismatch before Request

- Slurm: `FAILED`, batch/step exit `1:0`, elapsed `00:01:43`, nodes
  `itiger07-09`.
- Campaign: `spec168-campaign-v3-695ce1a81c32f9843ec6` (closed; never retry).
- Source: `sha256:f7c138b7dce961f6e22de18c1aa98fee04d78806f145de5cf354ecf45c829e9c`.
- Source bundle: `sha256:c5a0326c23be2c1e378dddf985215527f1c1dff2842885413178d324afd4482d`.

The source closure and policy transform executed, and the Controller started.
Its log proves that only the three compute Provider identities and one User
identity were admitted, so it generated four bootstrap tokens. The rank script
then requested tokens for three distinct `/repo`, `/repo/1`, and `/repo/2`
identities. Those identities were absent because the policy transform skipped
Repo services that already existed under compute Provider ownership.

All ranks exited before a Repo node, compute Provider, User Request, ACK, model
publication/fetch, or inference. This is a policy/launcher identity mismatch,
not a Repository throughput, CUDA, memory, model, or inference failure.

The failed directory initially contained one bootstrap-token file. It was
deleted without reading or copying its contents; `SECURITY-REDACTION.txt`
records the scrub. The replacement makes the policy reconciliation idempotent
for both new and already-existing Repo services and adds a local test requiring
exactly `/repo`, `/repo/1`, and `/repo/2` Provider ownership.

Retained SHA-256 values:

- `raw/node-0/policy-build.log`: `c645f64daaf34a2e9052835166ccbabdecc72ccc2143898f42dfc787b61de436`
- `raw/node-0/controller.log`: `97a871ea393db58ac55b9bea0705c8ae76425737ccc15ee3b38bc992862aabf2`
- `raw/SECURITY-REDACTION.txt`: `43d9a4a919f0d0564263a45c2e2c2b95d64ff3d4b1117cf7ba20cd27bd476f5f`
