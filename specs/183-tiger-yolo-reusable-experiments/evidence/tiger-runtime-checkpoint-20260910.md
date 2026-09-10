# Spec183 runtime checkpoint — 2026-09-10

This checkpoint records the exact sequence used for the current layered
candidate. It separates component evidence, real MiniNDN evidence, and the
formal local-owner failure; none of the latter is promoted to a PASS.

## Candidate and preflight

| Item | Recorded value |
| --- | --- |
| Base SIF | `base-runtime-controller-version-j4-v22-stable-20260909.sif`; SHA-256 `sha256:2c07a9f14d48fabd9fb58036c1634f3cc3282dd28c6470add9f8a7da0cb829b5`; 3,901,079,552 bytes |
| Base manifest | `/opt/ndnsf-di/current/manifest/base-runtime.json`; schema `spec183-base-runtime-v1`; six base-library artifacts; source seal `sha256:36b76454c869093ba87121773bbcd30fb133042b8fd149ddcf070eb32fb53296` |
| APP | v39, source revision `a93945554e0ba8a64338d53bbdc920984b1289a0`; source-seal digest `sha256:112c444243fe80eaa41e17410e6234590cce5f6c91714c5695e8f6ebcdda770d`; application manifest `sha256:f8af1b45cac7bf7bffbfa18737ebcaa62037e3813daacd2cf58064de94622813` |
| Profile | `Experiments/TigerCluster/profiles/yolo-two-node-controller-v48.json`; profile id `tiger-yolo-two-node-v1-v39` |
| Host receipt | `Experiments/TigerCluster/results/host-minindn-v40/host-minindn-v40.json`; status `PASS`; base/app hashes match the candidate |

The v36–v38 attempts remain retained failures: an APP cache directory was
mistaken for a bundle, the model package was incomplete, the oracle was staged
outside its manifest-bound package root, the extended negative schema was not
consumed by all validators, and the base-only source selection was rejected for
an APP build. The resulting rule is to use the actual base manifest, complete
the package payload, update every evidence consumer together, and use the
complete source selection for an external APP.

## Execution evidence

| Stage | Run and result | What it proves |
| --- | --- | --- |
| Real MiniNDN normal | `minindn-local-20260910-v105-v37`, Y-B, `RC=0`, `T010_DONE` | Four real processes, ACK/Selection, protected grant, terminal response, numeric oracle and clean cleanup |
| Real MiniNDN negative matrix | `minindn-local-20260910-v107-v37`, Y-N, `RC=0`, all 8 subcases PASS | Permission, placement, wrong-recipient/forged/expired grant, and bound post-Selection dependency evidence; extended fields include round, microbatch, operationKind and tensor |
| Formal exact-SIF local | `minindn-local-20260910-v110-v39`, candidate `sha256:ce8d574e3db3d9ac9a711510091fbfc949c6a2d04c198bf85280fea5a98454d7`, `RC=2` | Candidate, host receipt, network setup and APP integrity passed; the Controller child failed during its publication User's NAC-ABE public-parameter validation |
| Prior Tiger single-node | `210340` / `tiger-single-node-gpu-v35-r7`, PASS | Real CUDA model roles, CPU Merge, numeric oracle and cleanup on one node |
| Prior Tiger two-node normal | `210341` / `tiger-two-node-gpu-v35-r5`, PASS | Four roles across `itiger02`/`itiger03`, 1 warmup + 3 measured, nine dependency edges/request, CUDA/CPU backend evidence, oracle and cleanup |
| Prior Tiger negative | `210342` / `tiger-two-node-gpu-v35-neg1`, retained FAIL | Selection and two native withheld records were reached, but User observation/collection was lost at 59.9946 s and the graph exposed two same-role-pair logical edges |

## Formal local failure boundary

`node0/node-failure.json` records the Controller child exiting 139 while NFD
and finite role-command children were reaped. An isolated run with the exact
v22 SIF, v39 APP and v110 inputs reaches `SPEC180_CONTROLLER_READY` and then
aborts with exit 134:

```text
Fetched public parameters cannot be authenticated: Validator/policy did not invoke success or failure callback
```

This is a failure of the Controller-owned publication `ServiceUser` NAC-ABE
validator/policy callback contract after policy and permission exchange. It is
not evidence of a bad SIF hash, host-gate mismatch, NFD route failure, or CUDA
probe failure. T011 therefore remains open until a fresh formal local-owner run
completes publication, User observation, numeric comparison and cleanup.

## Required next order

1. Diagnose and fix the publication User NAC-ABE callback/policy contract; use
   a fresh run ID and invoke `prepare` followed directly by the formal `local`
   owner. Do not pre-create issuer/public/private or execution directories.
2. Preserve the existing `210340`/`210341` PASS records and repair the negative
   completion budget and unique logical cutpoint selection before a new T015
   allocation.
3. Run T015 and collect its complete User observation and cleanup receipt.
4. Only after T015 is `EXPECTED_REJECTION_PASS`, run T016 in a new two-node
   allocation with the unchanged profile, base SIF, APP, model and oracle.
