# Tasks: iTiger Multi-Node Qwen Collaboration

## Phase 1: Design and readiness

- [x] T001 Freeze the existing collaboration/Qwen code path, live cluster
  capacity, SIF/model identities, experiment contract, and pre-implementation
  audit in this Spec.

## Phase 2: Cross-node transport gate

- [x] T002 [US1] Implement in `jobs/nfd-multinode-probe.sbatch` and
  `jobs/analyze-nfd-probe.py`, locally validate, render, and exactly-once submit
  one three-node allocation probe that proves distinct-node GPU placement and
  job-local NFD named-Data exchange; preserve its complete terminal evidence
  under `results/spec160-itiger-multinode-qwen/`.

  **Attempt 001**: Executed as Slurm job `173197` and preserved as FAIL because
  `srun --exclusive` and `--overlap` were mutually exclusive. This attempt did
  not reach NFD startup and is not transport evidence. A corrected replacement
  requires a new linked submission identity and explicit authorization.

  **Attempt 002**: Explicitly authorized replacement executed as Slurm job
  `173250` and preserved as FAIL. All three NFDs reached readiness, but a second
  Slurm step could not see the first step's `/tmp` Unix socket. This is
  step-local filesystem isolation, not transport evidence. The next design uses
  one long-lived step per node for NFD, routing, producer, and consumer.

  **Attempt 003**: Explicitly authorized replacement executed as Slurm job
  `173251` and preserved as FAIL. The one-step design was submitted, but the
  copied rank launcher was not executable inside the evidence source bundle, so
  Slurm failed at `execve()` with `Permission denied` before NFD startup. This
  is a packaging failure, not transport evidence. The local next revision
  explicitly sets executable permissions in the job-local preserved source
  before `srun`.

  **Attempt 004**: Explicitly authorized replacement executed as Slurm job
  `173252` and preserved as FAIL. It allocated three distinct RTX 5000 nodes
  (`itiger07`, `itiger08`, `itiger09`) and entered the containerized rank flow,
  but NFD 24.07 rejected the harness config with `unknown module 'status' under
  authorize[0]` on all ranks. This is a harness configuration failure, not
  transport evidence. The next local revision removes the invalid `status`
  privilege from the probe-local `nfd.conf.in`.

  **Attempt 005**: Explicitly authorized replacement executed as Slurm job
  `173253` and preserved as FAIL. It reached NFD readiness on all three nodes,
  installed TCP/UDP routes on all ranks, and started the TCP producer/consumer,
  but the consumer timed out and the producer never observed the Interest. This
  is a Data-plane probe failure with useful evidence, but still not a PASS. The
  most likely harness issue is that the producer's `setInterestFilter()` app
  prefix registration was not confirmed before declaring producer readiness;
  the next local revision adds explicit producer registration success/failure
  logging and grants the probe-local config the `rib` privilege needed by app
  prefix registration.

  **Attempt 006**: Executed as Slurm job `173254` and preserved as FAIL. It
  failed before NFD readiness because NFD 24.07 rejected `rib` as an
  `authorizations.privileges` module. This is a harness configuration failure,
  not transport evidence. The next local revision removes that invalid
  privilege again, keeps producer registration success/failure logging, and
  moves the producer-ready barrier so it is written only after producer prefix
  registration succeeds.

  **Attempt 007**: Executed as Slurm job `173255` and preserved as FAIL. It
  reached NFD readiness and all TCP/UDP route creation commands eventually
  succeeded, but rank 0 hit `BARRIER_TIMEOUT:/shared/routes-ready-1` before rank
  1's route-ready marker became visible. This is a harness timing failure, not
  Data exchange evidence. The next local revision extends shared-file barrier
  waits from 30 seconds to 120 seconds.

  **Attempt 008**: Executed as Slurm job `173256` and preserved as FAIL. It
  reached NFD readiness, route readiness on all ranks, and producer prefix
  registration, but the TCP consumer timed out and the producer did not observe
  an incoming Interest. This is useful Data-plane evidence, but not a PASS. The
  next local revision simplifies the probe to isolate NFD forwarding by
  preloading signed Data into rank 0's local CS with `cs_unsolicited_policy
  admit-local`; rank 2 then fetches that exact Data through rank 1.

  **Attempt 009**: Executed as Slurm job `173257` and preserved as FAIL. Rank 0
  successfully preloaded signed Data into its local NFD CS, but rank 2 still
  timed out fetching `/spec160/tcp/173257`. The remaining harness issue is that
  the probe installed symmetric `/spec160/*` routes on the relay and endpoints.
  The next local revision uses a directional chain only: rank 2 routes to rank
  1, rank 1 routes to rank 0, and rank 0 only serves preloaded Data.

  **Attempt 010**: Executed as Slurm job `173258` and preserved as FAIL. The
  directional CS-preload probe proved TCP Data exchange across rank 2 -> rank 1
  -> rank 0 with `payloadMatch=true`, but UDP timed out. Rank 2 exit counters
  showed the UDP Interest left on the UDP face (`out=1`) with no returned Data
  (`in=0`). The next local revision switches `/spec160` to best-route and
  replaces producer-ready marker waits with a fixed 5-second post-route delay to
  reduce shared-filesystem and UDP face-readiness ambiguity.

  **Attempt 011**: Executed as Slurm job `173259` and preserved as FAIL. Rank 2
  received a TCP NACK reason `150` before rank 0 preloaded Data. This exposed a
  harness race: an expensive route-stage `nfdc status report` ran between route
  readiness and rank 0 preload. The next local revision removes that pre-probe
  status report so preload follows route readiness immediately.

  **Attempt 012**: Executed as Slurm job `173260` and preserved as FAIL. Rank 2
  again received TCP NACK reason `150`, showing the fixed 5-second post-route
  delay was still too short for shared-marker visibility and rank 0 preload on
  this allocation. The next local revision increases the consumer post-route
  delay to 30 seconds.

  **Attempt 013**: Executed as Slurm job `173261` and preserved as FAIL. The
  longer post-route delay removed the TCP race: rank 0 preloaded signed TCP
  Data and rank 2 fetched it through rank 1 with `payloadMatch=true` and
  `rttMs=0.743314`. The UDP leg still failed with `SPEC160_PROBE_NACK
  reason=150`; rank 2 exit counters show the UDP Interest left on the UDP face
  and a Nack returned, not payload Data. This proves the current TCP path but
  does not satisfy the requested TCP/UDP transport gate. The next replacement
  must diagnose whether raw inter-node UDP works inside the same Slurm
  allocation before further NFD UDP tuning.

  **Attempt 014**: Executed as Slurm job `173262` and preserved as FAIL. It
  added a raw UDP socket diagnostic before NFD startup. Rank 1 sent five UDP
  datagrams to rank 0, but rank 0's listener timed out; rank 1's listener also
  timed out waiting for rank 2 before Slurm terminated the failed step. This
  suggests inter-node UDP reachability may be blocked or unusable, but the
  first raw diagnostic used only a 20-second listener and scratch-copied logs,
  so the next replacement extends the listener to 120 seconds and writes raw
  UDP logs directly into shared evidence before deciding whether the substrate
  itself rejects UDP.

  **Attempt 015**: Executed as Slurm job `173263` and preserved as FAIL. The
  extended raw UDP diagnostic passed in both hops: rank 1 -> rank 0 and rank 2
  -> rank 1. This rules out a simple inter-node UDP firewall/substrate block
  for the allocated nodes. The NFD phase still failed when rank 2 received TCP
  NACK reason `150` for preloaded Data, showing the remaining issue is in the
  NFD probe timing/face readiness path rather than raw UDP reachability. The
  next replacement removes the NFD ready/route shared-file barriers from the
  critical timing path and uses fixed waits before consumers fetch.

  **Attempt 016**: Executed as Slurm job `173264` and preserved as PASS. It
  retained the raw UDP diagnostic, removed NFD ready/route shared-file barriers
  from the critical timing path, and used fixed settling delays before fetch.
  The allocation used three distinct nodes and GPUs (`itiger07`, `itiger08`,
  `itiger09`). Rank 2 fetched preloaded signed Data through rank 1 from rank 0
  over both TCP and UDP:
  - TCP: `payloadMatch=true`, `rttMs=0.965762`.
  - UDP: `payloadMatch=true`, `rttMs=1.80985`.
  Final evidence was promoted to
  `/project/tma1/ndnsf-di/evidence/spec160/probe/spec160-submission-nfd-3node-016/`
  and mirrored locally under
  `results/spec160-itiger-multinode-qwen/attempt-016/`.

## Phase 3: Frozen GPU stage artifacts

- [x] T003 [US2] Prepare through `jobs/prepare-qwen-stages.sbatch` or verify
  three checksum-bound FP16 Qwen stage packages
  in a bounded Slurm job, prove their exact layer coverage and CUDA loadability,
  and atomically promote their manifest plus `stage-manifest.json` without
  modifying the accepted SIF.

  **Stage Attempt 001**: Executed as Slurm job `173265` and preserved as FAIL.
  The job copied the intended 110-file source bundle, verified the source
  digest, then failed before model loading because the clean container could
  not import the `ndnsf` Python module required by the planning dataclasses:
  `ModuleNotFoundError: No module named 'ndnsf'`. This is a stage-prep harness
  dependency failure, not Qwen, CUDA, or artifact evidence. The next replacement
  adds an explicit prep-only `ndnsf` dataclass stub for policy serialization;
  T004 still must use the real NDNSF binding.

  **Stage Attempt 002**: Executed as Slurm job `173266` and preserved as PASS.
  It used the explicit prep-only `ndnsf` stub only for offline policy
  serialization, generated three FP16 `qwen-transformers` stage packages from
  the frozen Qwen model directory, and loaded each package on CUDA. Layer
  coverage is `[0,8)`, `[8,16)`, `[16,24)`, with one accepted artifact directory:
  `/project/tma1/ndnsf-di/artifacts/spec160/qwen-stages/spec160-qwen-stages-002`.
  Evidence was promoted to
  `/project/tma1/ndnsf-di/evidence/spec160/qwen-stages/spec160-qwen-stages-002/`
  and mirrored locally under
  `results/spec160-itiger-multinode-qwen/qwen-stages-002/`.

## Phase 4: Collaborative Qwen inference

- [x] T004a [US2] Preserve live Attempt 005 and runtime diagnostics as a
  blocked mixed-runtime baseline in `evidence/preflight.md`, including the
  Slurm job IDs, source digest, mounted override layout, provider abort text,
  and constructor stack showing failure at `NativeServiceProvider` before Qwen
  model preload.

- [x] T004b [US2] Define and build a clean coherent NDNSF-DI Qwen runtime in
  `packaging/ndnsf-di-container/` and Spec 160 evidence: one image/SIF must
  contain internally matched NDN-CXX, NAC-ABE/OpenABE, NDN-SVS, NDNSF,
  `ndnsf` Python binding, `py_repoclient` binding, NDNSF-DI Python package, and
  Qwen Python dependencies, with no formal live reliance on mounted replacement
  `.so`, pybind extension, or `vendor-site` directories.

  **Clean runtime build 001**: Executed locally through the Spec 158 layered
  build path as build ID `spec160-clean-runtime-20260727T215320Z`. The build
  reused the frozen ML and NDN foundation layers, rebuilt only the app runtime,
  and produced Docker image
  `ndnsf-di:spec158-spec160-clean-runtime-20260727T215320Z`
  (`sha256:16b99a0b82fb6ae4e98b1767cca3627eb1e322ed9a1493698666546caa5cbf69`).
  App source seal:
  `sha256:d6cbde0c2cbf992f4e74c2021f16c3b81f6cfd6978cde69ae24e7ccf72fd3e7c`;
  app lock:
  `sha256:a1670a77368a7b53783f43be95f7e2dc8f830da8925b4d7559c04217f2c5d51c`.
  The image build passed runtime library closure, static runtime probe, and
  local Python API constructor smoke. The constructor now fails only with the
  expected missing local identity configuration
  (`Cannot set non-existing identity ... as default`) and no longer aborts with
  double-free/invalid-pointer allocator corruption. Evidence is under
  `results/spec160-itiger-multinode-qwen/spec160-clean-runtime-20260727T215320Z/`.

- [x] T004c [US2] Submit a bounded Slurm runtime smoke for the rebuilt image
  that constructs `ServiceController`, `ServiceProvider`, and `ServiceUser`,
  imports Qwen runtime dependencies, and runs a single-node NDNSF-DI fake or
  Qwen smoke without allocator corruption, CPU fallback, or external runtime
  overrides; preserve evidence under `results/spec160-itiger-multinode-qwen/`.

  **Materialization Attempt 001**: Slurm job `173293` preserved as FAIL because
  Apptainer could not pull the GHCR digest from the compute node:
  `unable to retrieve auth token: invalid username/password: unauthorized`.
  This is registry-auth materialization evidence, not a runtime failure.

  **Archive Materialization Attempt 001**: Replacement Slurm job `173294`
  preserved as PASS. The clean image was streamed to
  `/project/tma1/ndnsf-di/images/spec160-178122a30d0a/candidate-docker-archive.tar.gz`
  with SHA-256
  `90fc608aa2eb3710b353891196c13c484af55363ef248ad52d6b578fd3982377`,
  then materialized from `docker-archive:` into
  `/project/tma1/ndnsf-di/releases/spec160-178122a30d0a-archive/runtime.sif`.
  SIF SHA-256:
  `8d8d95e7a5de86036ad62d972b911a66f2160553c6b3295d029dd3e7ae26106f`.

  **Runtime Smoke Attempts 001-003**: Slurm job `173295` preserved as FAIL due
  to an obsolete provider-ready log matcher in the harness. Slurm job `173296`
  passed but produced an ambiguous summary field, so it was not used to close
  T004c. Slurm job `173297` is the accepted PASS: it verified the SIF hash,
  started job-local NFD, constructed `ServiceController`, `ServiceProvider`,
  and `ServiceUser` through the Python API, completed one `/HELLO` request,
  imported `ndnsf`, `py_repoclient`, `ndnsf_distributed_inference`,
  `onnxruntime 1.20.1`, `torch 2.6.0+cu124`, and `transformers 4.48.2`,
  and scanned logs with `allocatorCorruption=false`. Evidence is mirrored under
  `results/spec160-itiger-multinode-qwen/runtime-smoke-178122a30d0a-003/`.

- [x] T004d [US2] Using only the rebuilt coherent runtime plus read-only frozen
  model/stage artifacts and writable evidence mounts, exactly-once submit one
  three-node secured NDNSF-DI Qwen collaboration request with one role provider
  per node; acceptance requires two cross-node dependency digests,
  requester-visible reference output, and zero CPU fallback.

  **Live Attempt 014**: Slurm job `173319` is preserved as FAIL. The request
  reached all three CUDA Qwen providers, but every collaboration handler failed
  while reporting its first operation-status snapshot because the Python
  binding attempted to construct `py::none` from a present string dictionary
  value. The requester timed out after `180513.514 ms`. The binding has been
  repaired locally and its native extension plus related tests pass. The
  coherent replacement image
  `ndnsf-di:spec158-spec160-operation-status-binding-20260728T052052Z-e`
  then passed a one-container, 4 GiB memory-limited collaboration smoke using
  NFD, Controller, one Provider, one User, and fake runtime. The real DI
  provider wrapper invoked `report_operation_status()`, the assignment reached
  selection-status states 1-4, and the fake response completed successfully.
  Evidence is under
  `results/spec160-itiger-multinode-qwen/local-docker-operation-status-20260728T070417Z/`.
  The exact image was then materialized by Slurm job `174124` into SIF
  `sha256:6638c813016e151fd6c19825db8dc01e3a326c23716084677eee3c4da935d52e`.
  SIF smoke jobs `174164` and `174170` are preserved as harness-only failures
  (`/usr/bin/time` missing, then runtime-journal locking on project NFS).
  Linked replacement job `174176` used compute-local scratch and passed in one
  Apptainer container under Slurm `ReqMem=5G`: one fake assignment completed
  at `50.376 ms`, selection-status states 1-4 were present, and no
  Qwen/Torch/CUDA/model mount, old binding error, token, or private key was
  retained. Accepted evidence is mirrored under
  `results/spec160-itiger-multinode-qwen/operation-status-sif-smoke-af0c3a3ce357-003-pass/`.
  These are local and single-node SIF binding gates, not T004d acceptance.

  **Live Attempt 015**: exactly-once submission `spec160-qwen-live-015` ran as
  Slurm job `174221` on `itiger07`, `itiger08`, and `itiger09`, explicitly
  replacing job `173319`. The secured request completed through three distinct
  RTX 5000 Ada GPU UUIDs with `device=cuda:0` and `cpuFallback=0`. Both
  cross-node hidden-state fetches completed; producer/consumer SHA-256 pairs
  matched at
  `784e075ed8db8a28ac108ae0849353f1a990afb164b1938c1b0a73fce0a134bf`
  and
  `d038b303945a672acadf6f7c04df9fc13c8c194817ed14e2b865cdceffa2170f`.
  The requester returned `status=ok` in `2794.666 ms`, with expected and actual
  top token `27024` and logits shape `[1, 5, 151936]`.

  The formal job is nevertheless preserved as `FAILED`, exit `1:0`, elapsed
  `00:07:07`: its post-run analyzer incorrectly compared the internal measured
  request ID `spec160-qwen-live-015-0` with the outer submission request ID
  `spec160-qwen-live-015`. A corrected analyzer passes read-only against the
  mirrored evidence, but the original result is not relabeled or promoted.
  All 58 original evidence checksums pass and no bootstrap token/private key
  was retained. The user then chose the no-rerun evidence-acceptance path:
  T004d closes from the immutable runtime evidence without changing job
  `174221` from its recorded `FAILED` terminal state. No Attempt 016 was
  submitted.

## Phase 5: Evidence closure

- [x] T005 [US3] Correlate node/GPU/role/layer/request/session/dependency/response
  evidence, audit every FR/SC, preserve negative outcomes, write
  `completion-summary.md`, and ring the completion bell.

## Dependencies

T001 -> T002 -> T003 -> T004a -> T004b -> T004c -> T004d -> T005. A failed
live gate stops all successors; there is no automatic rerun. Attempt 005 and
the provider-constructor diagnostics are preserved as mixed-runtime evidence,
not as Qwen execution evidence.
