# Spec183 runtime checkpoint — 2026-09-10

This checkpoint records the exact sequence used for the v22/v39 and v23/v49
layered candidates. It separates component evidence, real MiniNDN evidence,
formal local-owner failure, and the fresh local-owner PASS; no partial record is
promoted across a gate.

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

## Fresh v23/v49 local checkpoint — 2026-09-10

The failed v110 run was followed by a clean base rebuild from the complete
controller source selection. The base SIF is
`base-runtime-controller-version-j4-v23.sif`, 3,586,351,104 bytes as recorded by its
content-addressed cache, SHA-256
`sha256:44b44d564c64387716a17588ea387ee2a255948ee744855291c8e7a0676907b0`.
The source seal is
`sha256:3c8ffcd5146a34d7577fb3712ee9178bf46637adb0e4a6b453c6adbc76979642`.
The Controller and Producer startup callback is posted onto the Face IO
context; this removes the validator callback race seen in v110. The pybind
extensions were built with the bounded O0 fallback (`-O0 -g0 -B/usr/bin/` and
`BOOST_PHOENIX_DONT_USE_PREPROCESSED_FILES`) after the GCC9 O1 compiler ICE.

The external APP remains v39, source seal
`sha256:112c444243fe80eaa41e17410e6234590cce5f6c91714c5695e8f6ebcdda770d`,
manifest SHA
`sha256:4a6c3af0c2cc24620c28519404f1a472074ebf354608372934339ae241942db7`,
and build jobs=4. It is mounted read-only over the unchanged base libraries;
no host library is injected. The candidate digest in the local verdict is
`sha256:ed7967c65e6c765f74f4ad64fd45154b5094dbdc380a03a49a75eee0bd985384`.

The exact command was:

```bash
python3 Experiments/TigerCluster/jobs/yolo/submit.py local \
  --profile /project/tma1/ndnsf-di/candidates/spec183-v49-20260910/profile-v49.json \
  --run-id tiger-local-cpu-v49-r1 \
  --output /project/tma1/ndnsf-di/runs --case local-cpu
```

The retained verdict is
`/project/tma1/ndnsf-di/runs/tiger-local-cpu-v49-r1/verdict.json`, 226050 bytes,
SHA-256
`sha256:d15b41c1e853584de893d996741b31d9ee95b4ec89804e0373ad623486c17ebe`.
It reports `status=PASS`, `qualification=NORMAL_EXPERIMENT_PASS`, two requests,
four providers and nine dependency edges per request. Both numerical oracles
have shape `[1,50,6]`, `matched=true`, `atol=0.001`, `rtol=0.0001`, and
`maxAbsError=0.0005340576171875`; CPU roles report no fallback and all child
cleanup records are controlled. This closes the formal local gate T011 only.

The first v49 Tiger submission uses the same candidate and profile:
`tiger-single-node-gpu-v49-r7`. Its transport initially stopped at
`REMOTE_STATE_UNRESOLVED` because the declared project candidate root had not
been created. After creating that root, the same run resumed with rsync
append/verify; no second Slurm submission was issued. Until a Slurm job ID,
compute node, CUDA probe, terminal verdict and cleanup receipt are present, the
run is `RUNNING`, not GPU PASS.

## Fresh v49 Tiger GPU and negative results

The remote execution continued with append-only profile records. The base SIF
and APP bytes stayed unchanged; only the profile gate references and runtime
run IDs changed. The following table records the terminal boundary for every
attempt, including transport failures that never reached Slurm.

| Run | Slurm / hosts | Terminal evidence | Result |
| --- | --- | --- | --- |
| `tiger-single-node-gpu-v49-r7` | `210364` / `itiger02` | Provider launch returned 126: `/usr/bin/env: '/app/bin/di-native-provider': Permission denied`; the transported APP binaries were `0444` | FAIL before request; executable modes restored to `0555` |
| `tiger-single-node-gpu-v49-r8` | `210365` / `itiger02` | Verdict 230987 bytes, SHA `sha256:e7e2809fee7623c6131ec64d3c7df7c6b7994b3fb40c61bb6f830f9061d19649`; candidate `sha256:00db0251e0e4eb98f0b8067fc5c2dcd9d3d55778c7bea5365c11932241646922`; two requests; model roles CUDA, Merge CPU; shape `[1,50,6]`, `maxAbsError=0.00042724609375`; GPU UUID `GPU-519e5825-d84a-8e55-670c-c53433c4a71c`; cleanup closed | `NORMAL_EXPERIMENT_PASS`; closes T013/V15 |
| `tiger-two-node-gpu-v49-r9` | no job | `SSH_DESTINATION_CONFLICT` against the immutable remote candidate/profile root | FAIL in transport |
| `tiger-two-node-gpu-v49-r10` | no job | `FILE_SIZE_OR_TYPE:hostMinindn` after a rewritten host gate retained stale bytes/hash | FAIL in transport preflight |
| `tiger-two-node-gpu-v49-r11` | no job | `TRANSPORT_OUTSIDE_ROOTS` because retained r8 gate references still named the old root | FAIL in transport preflight |
| `tiger-two-node-gpu-v49-r12` | no job | Remote `.incoming` was accidentally `0555`; staging raised `PermissionError` | FAIL in receiver staging; mode restored to `0700` |
| `tiger-two-node-gpu-v49-r13` | `210366` / `itiger02`, `itiger03` | Verdict 464828 bytes, SHA `sha256:c8e4487127096e938f438745746b7735dc6082c502389b010d3a48f325916e92`; candidate `sha256:08c7df48dfa6e89bcbd54dab9b061c5f861394121dd6eb012fd596216e0d970d`; four requests (one warmup + three measured), four roles, nine edges/request, both GPU UUIDs, shape `[1,50,6]`, `maxAbsError=0.00042724609375`, no CPU fallback, cleanup closed | `NORMAL_EXPERIMENT_PASS`; closes T014/V16 |
| `tiger-negative-dependency-v49-r14` | `210373` / `itiger05`, `itiger06` | `srun.log` SHA `sha256:c623cc08a511a050467f87e343daed5f8976c686f705bab043657640eb946d51`; Selection committed; DetectShard0 wrote one bound `NDNSF_DI_OUTPUT_WITHHELD`; Merge wrote exact `NDNSF_DI_NATIVE_FAILURE` for the signed Data name. Rank1 had already published `workload-complete` while rank0 was still in cold request preparation; rank1 then failed with `TimeoutError('STARTUP_DEADLINE')`. No `negative-user.json`, `collection-input.json` or verdict was produced | `FAILED_BOUNDARY`; T015 remains open |

The r14 failure is an operator-harness budget defect. `completion_seconds` for
one negative request was 120 seconds (`90` seconds process budget plus `30`
seconds cleanup), but the rank1 completion clock started at its
`providers-ready` record (`23:33:54`) while rank0 did not write the User
request lifecycle until `23:35:12`. The native dependency failure itself was
observed at `23:35:45`; the outer completion deadline expired at `23:35:54`
before the User observer could write its post-shutdown record. This is not a
SIF, CUDA, transport or DI numerical failure. The corrective action is to
reserve the cold preparation interval in the negative completion budget (or
arm the barrier after both ranks are ready), then run one new T015 allocation.
No base SIF rebuild is required for that harness-only correction.

## Reproducible order after this checkpoint

1. Keep the v23 SIF and v49 APP immutable; use the canonical candidate root and
   append-only profile gate records.
2. Verify candidate inventory, executable APP modes, remote `.incoming` mode,
   host-gate bytes/hashes and transport-root closure before `sbatch`.
3. Run `prepare`, then one exact local owner; collect only after the scheduler
   terminal state and both rank receipts are present.
4. For normal GPU qualification, require single-node T013, first normal
   two-node T014, then a separate negative T015. Do not count transport or
   component records as runtime PASS.
5. Retry T015 with the corrected completion budget and require all of
   `negative-user.json`, one logical withheld edge, exact Merge native failure,
   no response/reselection and clean cleanup. Only then run T016 with a new
   two-node allocation and unchanged base/APP/profile/model/oracle.
