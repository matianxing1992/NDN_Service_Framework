# Spec186 Apptainer runtime policy receipt

This receipt records the runtime policy requested for Spec186: local
NDNSF-DI SIF work uses the only local Apptainer installation, 1.5.3; Tiger
SIF execution uses the allocated compute-node 1.5.3 binary. The Tiger login
node's 1.3.4 package is retained only as an SSH/Slurm metadata boundary and is
never an execution fallback.

| Scope | Explicit executable | Required version | Verification |
| --- | --- | --- | --- |
| Local MiniNDN/SIF | `/usr/local/bin/apptainer` | `1.5.3` | `apptainer --version` → `apptainer version 1.5.3` |
| Tiger compute SIF | `/usr/bin/apptainer` | `1.5.3` (package suffix permitted) | bounded `srun ... /usr/bin/apptainer --version` → `apptainer version 1.5.3-1.el9` |
| Tiger login | metadata only | not an execution target | observed `1.3.4-1.el9`; no SIF command is run there |

All eight `spec184-*.json` profiles now carry
`runtime.apptainer.path` and `runtime.apptainer.version`. The candidate
manifest includes this pair, the effective argv uses the declared executable
instead of PATH lookup, and local pre-dispatch verifies the executable with
the bounded `--version` form. The compute preflight rejects any version other
than the 1.5.3 package line.

The same profile refresh corrected the r5 application tree digest to
`687610de859155449c51ec2ba4bb7b57c77614cbf0a53f106bb65152f8c07129` and bound
the current collector digest
`d42d7859bff1838f3293e4925db56e0d8ab46bb72a2b711a7bc7d61073ae9a1d`.
Fresh offline candidate generation produced these identities:

| Profile | Candidate digest | First blocking boundary |
| --- | --- | --- |
| `spec184-qwen06b-minindn-cpu` | `2feb4024da630d0e2859c051847d80c842f8a0521a56e6a5d5dcc0faef01436e` | missing base SIF and Qwen3 tuple |
| `spec184-qwen06b-tiger-experimental` | `ae6cb6ffb876b4bd37331615be73a680c0a162ca13f42f9cd9a4f9437fd93cbd` | local visibility plus missing base SIF/Qwen3 tuple |
| `spec184-yolo-minindn-negative` | `6ac06c5ecee83b7f331b06cb4604280e92b753d64aac5116a816030fed7d7a00` | missing base SIF |
| `spec184-yolo-minindn-normal` | `1fc709a635755aef760f1a44c1dada9235c47318123911155760e5ce6792483f` | missing base SIF |
| `spec184-yolo-tiger-single-gpu` | `9de4a239fe95dc139d2540d98a56b9853a8dd7e72c21ad2986cdd5ee76b74d46` | Tiger-local visibility plus missing base SIF/model |
| `spec184-yolo-tiger-two-node-negative` | `bb40b6dbb9f852ce34a93560fa89eb8bc943fe4dcf3a35bcd2092fa8b18d7c28` | Tiger-local visibility plus missing base SIF/model |
| `spec184-yolo-tiger-two-node-normal` | `a3a4034a5e78adb5c3257bab4ed4b94c323493692a45ed8b8a96a66eb70a5eb0` | Tiger-local visibility plus missing base SIF/model |
| `spec184-yolo-tiger-two-node-reuse` | `eb97e5a593c826031fca059c3ccd90f7ec5342df244ec8b98a00718101116d5c` | Tiger-local visibility plus missing base SIF/model |

The profile checks and Tiger adapter tests pass. These receipts remain
pre-dispatch evidence only; the exact source-sealed base SIF, local route
repair, Qwen3 tuple and runtime campaign are still required for qualification.
