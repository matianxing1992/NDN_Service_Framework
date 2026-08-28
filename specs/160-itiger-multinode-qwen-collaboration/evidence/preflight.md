# Preflight and Attempt Ledger

**Captured**: 2026-07-27

## Verified before implementation

- VPN and batch SSH: connected.
- Partition/account/QOS: `bigTiger` / `devs` / `normal`.
- Live inventory: five RTX 5000 nodes, eight GPUs per node.
- Association capacity from Spec 159: up to three nodes and 24 GPUs.
- Reused SIF:
  `/project/tma1/ndnsf-di/releases/spec159-3ac97b8792ff302d/runtime.sif`,
  SHA-256
  `7e904e7fe7502957277ee28778957f781c90f29c8299518c477de129e20b9284`.
- Frozen model revision:
  `7ae557604adf67be50417f59c2c2f167def9a775`.
- Existing Qwen pipeline: three roles covering layers `[0,8)`, `[8,16)`,
  `[16,24)` with planned segmented dependency Data.
- CUDA placement gap: current runner loaded stage packages and incoming hidden
  states on CPU. A fail-closed explicit device option and three local tests
  were added before any model job.

## Attempt 001: cross-node NFD probe

- Submission: `spec160-submission-nfd-3node-001`.
- Run: `spec160-run-nfd-3node-001`.
- Source bundle SHA-256:
  `e77dfaa7f6d5283724b616bb245165d3ef4257d2966ef78ff53e1c311e313b38`.
- Slurm job: `173197`.
- Allocation: `itiger07`, `itiger08`, `itiger09`; three RTX 5000 GPUs.
- Terminal state: `FAILED`, exit `1:0`, elapsed `00:00:39`.
- Root cause: all node steps rejected mutually exclusive
  `srun --exclusive --overlap`; NFD never started.
- Authority: harness failure only. It proves neither success nor failure of
  cross-node NFD transport.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/probe/.spec160-submission-nfd-3node-001.partial/`.

The corrected script removes `--exclusive`. It has not been submitted. Any
replacement must use a new linked identity; Attempt 001 remains immutable.

## Attempt 002: first corrected replacement

- Submission: `spec160-submission-nfd-3node-002`.
- Run: `spec160-run-nfd-3node-002`.
- Source bundle SHA-256:
  `3ad1931b075b50959a17f5421b3ba41b9d39fdb2962d3bbe48b4dc0727a4f6e7`.
- Replaces failed job: `173197`.
- Slurm job: `173250`.
- Allocation: `itiger07`, `itiger08`, `itiger09`; three RTX 5000 GPUs.
- Terminal state: `FAILED`, exit `1:0`, elapsed `00:00:45`.
- Reached: three distinct hosts/GPUs and all three NFD readiness markers.
- Root cause: the later route-management Slurm step could not see the
  NFD-owning step's `/tmp/.../nfd.sock`; Slurm provides isolated temporary
  state across these steps.
- Authority: harness isolation failure only. No inter-node face or Data probe
  was executed.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/probe/.spec160-submission-nfd-3node-002.partial/`.

The next local revision eliminates overlapping Slurm steps. One three-rank
`srun` keeps NFD, route setup, producer, and consumer inside one long-lived
process/container per node. It has not been submitted.

## Attempt 003: one-step replacement

- Submission: `spec160-submission-nfd-3node-003`.
- Run: `spec160-run-nfd-3node-003`.
- Source bundle SHA-256:
  `6b93bc9dc7c413d0550492e5fa7a1d6680e5acc943fc9ea3dfde27966449061d`.
- Replaces failed job: `173250`.
- Slurm job: `173251`.
- Allocation: `itiger07`, `itiger08`, `itiger09`; three RTX 5000 GPUs.
- Terminal state: `FAILED`, exit `15:0`, elapsed `00:00:09`.
- Root cause: `srun` failed at `execve()` because
  `.spec160-submission-nfd-3node-003.partial/source/nfd-probe-rank.sh` was not
  executable after being copied into the preserved evidence source bundle.
- Authority: harness packaging failure only. NFD did not start, and no TCP/UDP
  Data probe was executed.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/probe/.spec160-submission-nfd-3node-003.partial/`.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/attempt-003/`.

The next local revision keeps the one-step design and adds explicit `chmod 700`
for the preserved rank launchers and probe binary before `srun`. It also copies
`ndn-data-probe.cpp` into the preserved source bundle. It has not been
submitted.

## Attempt 004: executable one-step replacement

- Submission: `spec160-submission-nfd-3node-004`.
- Run: `spec160-run-nfd-3node-004`.
- Source bundle SHA-256:
  `408f8140ac596d1117d85a703189e9b849bbc88ace94625b69d36d98740fc9ce`.
- Replaces failed job: `173251`.
- Slurm job: `173252`.
- Allocation: `itiger07`, `itiger08`, `itiger09`; three RTX 5000 GPUs.
- Observed node/GPU mapping:
  - rank 0: `itiger07`, `172.16.0.17`,
    `GPU-25e44831-503d-7227-29f7-d2346fbb1d80`.
  - rank 1: `itiger08`, `172.16.0.18`,
    `GPU-b76fa0fa-afae-8df6-a44c-158ccf9aa38b`.
  - rank 2: `itiger09`, `172.16.0.19`,
    `GPU-84ba2af4-da27-b890-5bbd-f3bd1d33e718`.
- Terminal state: `FAILED`, exit `5:0`, elapsed `00:00:15`.
- Root cause: NFD 24.07 rejected the probe-local config with
  `unknown module 'status' under authorize[0]` on all three ranks.
- Authority: harness configuration failure only. NFD did not reach readiness,
  and no TCP/UDP Data probe was executed.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/probe/.spec160-submission-nfd-3node-004.partial/`.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/attempt-004/`.

The next local revision removes the invalid `status` privilege from
`nfd.conf.in`. It has not been submitted.

## Attempt 005: route-installed Data-plane probe

- Submission: `spec160-submission-nfd-3node-005`.
- Run: `spec160-run-nfd-3node-005`.
- Source bundle SHA-256:
  `27da48d2cce0b5c85118318e1ea9b4b4b6b0fc72a464543a97306a0a20eb8926`.
- Replaces failed job: `173252`.
- Slurm job: `173253`.
- Allocation: `itiger07`, `itiger08`, `itiger09`; three RTX 5000 GPUs.
- Terminal state: `FAILED`, exit `5:0`, elapsed `00:01:23`.
- Reached:
  - NFD readiness on all three ranks.
  - TCP and UDP face creation and route installation on all three ranks.
  - TCP producer and TCP consumer launch.
- Root cause observed: TCP consumer timed out for `/spec160/tcp/173253`;
  producer exited without `SPEC160_PROBE_PRODUCED`; relay rank timed out waiting
  for `tcp-producer-done`.
- Interpretation: this is the first useful Data-plane probe failure. It proves
  multi-node NFD startup and management route installation, but it does not
  prove payload exchange. The likely harness gap is unconfirmed producer app
  prefix registration before declaring producer readiness.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/probe/.spec160-submission-nfd-3node-005.partial/`.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/attempt-005/`.

The next local revision adds explicit producer prefix-registration
success/failure logging to `ndn-data-probe.cpp` and grants `rib` in
`nfd.conf.in` for app prefix registration. It has not been submitted.

## Attempt 006: invalid RIB privilege diagnostic

- Submission: `spec160-submission-nfd-3node-006`.
- Run: `spec160-run-nfd-3node-006`.
- Source bundle SHA-256:
  `21251fde123821dd00f506231ea488ca23bd930f784834a4d3fc6193266f4841`.
- Replaces failed job: `173253`.
- Slurm job: `173254`.
- Allocation: `itiger07`, `itiger08`, `itiger09`; three RTX 5000 GPUs.
- Terminal state: `FAILED`, exit `5:0`, elapsed `00:00:10`.
- Root cause: NFD 24.07 rejected the probe-local config with
  `unknown module 'rib' under authorize[0]` on all three ranks.
- Authority: harness configuration failure only. NFD did not reach readiness.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/probe/.spec160-submission-nfd-3node-006.partial/`.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/attempt-006/`.

The next local revision removes the invalid `rib` privilege again, keeps
producer registration success/failure logging, and makes the producer write the
`producer-ready` barrier only after prefix registration succeeds. It has not
been submitted.

## Attempt 007: producer-registration barrier diagnostic

- Submission: `spec160-submission-nfd-3node-007`.
- Run: `spec160-run-nfd-3node-007`.
- Source bundle SHA-256:
  `a2fb6f0f8991175d13f05c65820d0e456b217543493ce11fb7cde5bce13d7b4b`.
- Replaces failed job: `173254`.
- Slurm job: `173255`.
- Allocation: `itiger07`, `itiger08`, `itiger09`; three RTX 5000 GPUs.
- Terminal state: `FAILED`, exit `9:0`, elapsed `00:01:10`.
- Reached:
  - NFD readiness on all three ranks.
  - TCP and UDP route creation eventually succeeded on all ranks.
- Root cause: rank 0 timed out waiting for `/shared/routes-ready-1` after 30
  seconds; rank 1 route creation completed just after the timeout path had
  already terminated the step.
- Authority: harness timing failure only. No TCP/UDP payload exchange was
  attempted.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/probe/.spec160-submission-nfd-3node-007.partial/`.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/attempt-007/`.

The next local revision extends shared-file barrier waits from 30 seconds to
120 seconds. It has not been submitted.

## Attempt 008: producer-registered TCP timeout

- Submission: `spec160-submission-nfd-3node-008`.
- Run: `spec160-run-nfd-3node-008`.
- Source bundle SHA-256:
  `e06594bde565b5f7173ca6feb147445b7708b3e03590428eb30414a2fa85db10`.
- Replaces failed job: `173255`.
- Slurm job: `173256`.
- Allocation: `itiger07`, `itiger08`, `itiger09`; three RTX 5000 GPUs.
- Terminal state: `FAILED`, exit `9:0`, elapsed `00:01:30`.
- Reached:
  - NFD readiness on all three ranks.
  - TCP and UDP route readiness on all ranks.
  - Producer prefix registration:
    `SPEC160_PROBE_REGISTERED name=/spec160/tcp/173256`.
- Root cause observed: rank 2 TCP consumer timed out for
  `/spec160/tcp/173256`; rank 0 producer did not observe an incoming Interest.
- Authority: useful Data-plane failure evidence, but not a transport PASS.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/probe/.spec160-submission-nfd-3node-008.partial/`.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/attempt-008/`.

The next local revision isolates NFD forwarding by preloading signed Data into
rank 0's local CS with `cs_unsolicited_policy admit-local`, then fetching that
exact Data from rank 2 through rank 1. It has not been submitted.

## Attempt 009: CS preload with symmetric routes

- Submission: `spec160-submission-nfd-3node-009`.
- Run: `spec160-run-nfd-3node-009`.
- Source bundle SHA-256:
  `8848ad10411b2ba354eae6ffb99dbb78296a8b347e7c07307664546de41ffcb0`.
- Replaces failed job: `173256`.
- Slurm job: `173257`.
- Allocation: `itiger07`, `itiger08`, `itiger09`; three RTX 5000 GPUs.
- Terminal state: `FAILED`, exit `9:0`, elapsed `00:02:22`.
- Reached:
  - NFD readiness and route readiness on all ranks.
  - Rank 0 preloaded signed Data:
    `SPEC160_PROBE_PRELOADED name=/spec160/tcp/173257 bytes=18`.
- Root cause observed: rank 2 consumer timed out for `/spec160/tcp/173257`.
- Interpretation: CS preload isolated producer registration out of the failure,
  but symmetric `/spec160/*` routes still make the probe ambiguous.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/probe/.spec160-submission-nfd-3node-009.partial/`.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/attempt-009/`.

The next local revision installs a directional chain only: rank 2 routes to rank
1, rank 1 routes to rank 0, and rank 0 only serves preloaded Data. It also
captures `nfd-status-exit.txt` during cleanup. It has not been submitted.

## Attempt 010: directional TCP pass, UDP timeout

- Submission: `spec160-submission-nfd-3node-010`.
- Run: `spec160-run-nfd-3node-010`.
- Source bundle SHA-256:
  `930694a9d57bf52d81a834dfb428bf2bf186c77287d64217d7f79f2b67befe12`.
- Replaces failed job: `173257`.
- Slurm job: `173258`.
- Allocation: `itiger07`, `itiger08`, `itiger09`; three RTX 5000 GPUs.
- Terminal state: `FAILED`, exit `9:0`, elapsed `00:02:20`.
- Reached:
  - Directional route chain rank 2 -> rank 1 -> rank 0.
  - Rank 0 CS preload for TCP and UDP.
  - TCP fetch passed:
    `SPEC160_PROBE_CONSUMED name=/spec160/tcp/173258 bytes=18 payloadMatch=true rttMs=0.956762`.
- Root cause observed: UDP fetch timed out:
  `SPEC160_PROBE_TIMEOUT name=/spec160/udp/173258`.
- Additional evidence: rank 2 exit counters showed TCP face `out=1/in=1`,
  while UDP face had `out=1/in=0`; the UDP Interest left rank 2 but no Data
  returned.
- Authority: TCP cross-node Data path is proven for this allocation; the
  originally requested TCP/UDP gate is not yet satisfied.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/probe/.spec160-submission-nfd-3node-010.partial/`.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/attempt-010/`.

The next local revision switches `/spec160` to best-route and replaces
producer-ready marker waits with a fixed 5-second post-route delay before
consumer fetch, reducing shared-filesystem and UDP face-readiness ambiguity. It
has not been submitted.

## Attempt 011: consumer-before-preload race

- Submission: `spec160-submission-nfd-3node-011`.
- Run: `spec160-run-nfd-3node-011`.
- Source bundle SHA-256:
  `7c32ab0950034f73fe6c134d3e4054dc7d9b88704a78ecf0aef7391a65eb0f47`.
- Replaces failed job: `173258`.
- Slurm job: `173259`.
- Allocation: `itiger07`, `itiger08`, `itiger09`; three RTX 5000 GPUs.
- Terminal state: `FAILED`, exit `9:0`, elapsed `00:01:15`.
- Root cause observed: rank 2 received `SPEC160_PROBE_NACK reason=150` before
  rank 0 had preloaded Data.
- Interpretation: the pre-probe `nfdc status report` after route readiness
  delayed rank 0 preload while rank 2 had already entered its fixed-delay
  consumer path.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/probe/.spec160-submission-nfd-3node-011.partial/`.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/attempt-011/`.

The next local revision removes the pre-probe `nfdc status report` so rank 0
preloads immediately after route readiness. It has not been submitted.

## Attempt 012: fixed delay still before preload

- Submission: `spec160-submission-nfd-3node-012`.
- Run: `spec160-run-nfd-3node-012`.
- Source bundle SHA-256:
  `52073f861855433103cd84fc9b809175a785e31f588539f9c18cb5c9e55e6951`.
- Replaces failed job: `173259`.
- Slurm job: `173260`.
- Allocation: `itiger07`, `itiger08`, `itiger09`; three RTX 5000 GPUs.
- Terminal state: `FAILED`, exit `9:0`, elapsed `00:01:16`.
- Root cause observed: rank 2 again received `SPEC160_PROBE_NACK reason=150`.
- Interpretation: removing the pre-probe status report was insufficient; the
  5-second post-route consumer delay was still too short for rank 0 preload in
  this shared-filesystem/barrier environment.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/probe/.spec160-submission-nfd-3node-012.partial/`.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/attempt-012/`.

The next local revision increases rank 2's post-route consumer delay to 30
seconds. It has not been submitted.

## Attempt 013: TCP pass with delayed consumer, UDP NACK

- Submission: `spec160-submission-nfd-3node-013`.
- Run: `spec160-run-nfd-3node-013`.
- Source bundle SHA-256:
  `16cd8baf8f38dee792f21dd7aaefe722e0f252acb25636adf999a9b51856cf23`.
- Replaces failed job: `173260`.
- Slurm job: `173261`.
- Allocation: `itiger07`, `itiger08`, `itiger09`; three RTX 5000 GPUs.
- Terminal state: `FAILED`, exit `9:0`, elapsed `00:02:40`; rank step exit
  `4:0`, elapsed `00:02:31`.
- Reached:
  - NFD readiness on all three ranks.
  - Directional TCP and UDP routes on rank 2 -> rank 1 -> rank 0.
  - Rank 0 CS preload for TCP and UDP.
  - TCP fetch passed:
    `SPEC160_PROBE_CONSUMED name=/spec160/tcp/173261 bytes=18 payloadMatch=true rttMs=0.743314`.
- Root cause observed: UDP fetch failed with `SPEC160_PROBE_NACK reason=150`.
- Additional evidence: rank 2 exit counters show UDP face
  `out={1i 0d 0n}` and `in={0i 0d 1n}`, meaning the UDP Interest was emitted
  and a Nack returned, but no Data returned. TCP on the same allocation returned
  Data successfully.
- Authority: the TCP cross-node path is proven for this allocation; the
  originally requested TCP/UDP gate is still not satisfied.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/probe/.spec160-submission-nfd-3node-013.partial/`.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/attempt-013/`.

The next replacement should first run a minimal raw UDP socket check inside the
same three-node Slurm allocation, alongside the existing NFD TCP/UDP probe, to
separate substrate UDP reachability from NFD UDP face behavior. It has not been
submitted.

## Attempt 014: first raw UDP substrate diagnostic

- Submission: `spec160-submission-nfd-3node-014`.
- Run: `spec160-run-nfd-3node-014`.
- Source bundle SHA-256:
  `45853a7b119d0c455cbd8ad067e26f233124e706088ff5b20ce66f93c246c4fc`.
- Replaces failed job: `173261`.
- Slurm job: `173262`.
- Terminal state: `FAILED`, exit `9:0`, elapsed `00:01:02`; rank step exit
  `4:0`, elapsed `00:00:50`.
- Reached:
  - Three distinct node records before NFD startup.
  - Raw UDP listener readiness markers on rank 0 and rank 1.
  - Rank 1 sent five UDP datagrams to rank 0:
    `SPEC160_RAW_UDP_SEND attempt=1..5 to=172.16.0.17:33263 payload=rank1-to-rank0`.
- Root cause observed:
  - Rank 0 timed out waiting for `rank1-to-rank0`.
  - Rank 1 timed out waiting for `rank2-to-rank1`.
- Authority: useful raw substrate diagnostic, but not yet a definitive UDP
  substrate verdict because the first diagnostic used a 20-second listener and
  copied raw logs from scratch only during cleanup.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/probe/.spec160-submission-nfd-3node-014.partial/`.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/attempt-014/`.

The next replacement extends the raw UDP listener timeout to 120 seconds and
writes raw UDP send/listen logs directly into shared evidence before NFD starts.
It has not been submitted.

## Attempt 015: raw UDP passes, NFD TCP still NACKs

- Submission: `spec160-submission-nfd-3node-015`.
- Run: `spec160-run-nfd-3node-015`.
- Source bundle SHA-256:
  `c43b94017f3c7a046ea914211b321753b68bc7d2a31cbdbc8a5742720d8058ba`.
- Replaces failed job: `173262`.
- Slurm job: `173263`.
- Terminal state: `FAILED`, exit `9:0`, elapsed `00:03:19`; rank step exit
  `4:0`, elapsed `00:03:02`.
- Reached:
  - Raw UDP rank 1 -> rank 0 passed:
    `SPEC160_RAW_UDP_RECV from=172.16.0.18 ... payload=rank1-to-rank0`.
  - Raw UDP rank 2 -> rank 1 passed:
    `SPEC160_RAW_UDP_RECV from=172.16.0.19 ... payload=rank2-to-rank1`.
  - NFD readiness and directional route creation on all ranks.
  - Rank 0 preloaded TCP Data:
    `SPEC160_PROBE_PRELOADED name=/spec160/tcp/173263 bytes=18`.
- Root cause observed: rank 2 received `SPEC160_PROBE_NACK reason=150` for the
  TCP Data fetch.
- Authority: raw inter-node UDP reachability is proven for this allocation;
  the NFD TCP/UDP gate is still not satisfied because the NFD probe continues
  to race consumer fetch against route/preload readiness.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/probe/.spec160-submission-nfd-3node-015.partial/`.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/attempt-015/`.

The next replacement removes shared-file NFD ready/route barriers from the
critical path and uses fixed NFD and route settling delays before rank 2
consumes. It has not been submitted.

## Attempt 016: cross-node NFD TCP/UDP probe accepted

- Submission: `spec160-submission-nfd-3node-016`.
- Run: `spec160-run-nfd-3node-016`.
- Source bundle SHA-256:
  `86ea9721402521e12e3efde368980021480135a4c2ec4f5303af2c1b03377a59`.
- Replaces failed job: `173263`.
- Slurm job: `173264`.
- Allocation: `itiger07`, `itiger08`, `itiger09`; three RTX 5000 GPUs:
  - rank 0: `GPU-25e44831-503d-7227-29f7-d2346fbb1d80`.
  - rank 1: `GPU-b76fa0fa-afae-8df6-a44c-158ccf9aa38b`.
  - rank 2: `GPU-84ba2af4-da27-b890-5bbd-f3bd1d33e718`.
- Terminal state: `COMPLETED`, exit `0:0`, elapsed `00:04:28`; rank step
  completed with exit `0:0`.
- Reached:
  - Raw UDP rank 1 -> rank 0 passed.
  - Raw UDP rank 2 -> rank 1 passed.
  - NFD readiness on all ranks.
  - Directional route chain rank 2 -> rank 1 -> rank 0.
  - Rank 2 fetched rank 0 preloaded signed Data through rank 1 over TCP:
    `SPEC160_PROBE_CONSUMED name=/spec160/tcp/173264 bytes=18 payloadMatch=true rttMs=0.965762`.
  - Rank 2 fetched rank 0 preloaded signed Data through rank 1 over UDP:
    `SPEC160_PROBE_CONSUMED name=/spec160/udp/173264 bytes=18 payloadMatch=true rttMs=1.80985`.
- Authority: allocation-scoped three-node NFD TCP/UDP transport gate passed.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/probe/spec160-submission-nfd-3node-016/`.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/attempt-016/`.

T002 is closed. The next gate is T003: prepare or verify three checksum-bound
FP16 Qwen stage packages without modifying the accepted SIF.

## Stage Attempt 001: Qwen stage-prep missing ndnsf binding

- Submission: `spec160-qwen-stages-001`.
- Run: `spec160-run-qwen-stages-001`.
- Source bundle SHA-256:
  `778e1c3e663418ba1b9c6b0566a1054aaec0246edd4562e292e822df08f9f88b`.
- Slurm job: `173265`.
- Terminal state: `FAILED`, exit `1:0`, elapsed `00:01:17`.
- Reached:
  - Single RTX 5000 allocation.
  - 110-file source bundle copied into evidence.
  - Accepted SIF checksum verified.
- Root cause: `plan_pipeline.py` failed importing
  `ndnsf_distributed_inference.plan` because the clean container did not expose
  `ndnsf.CollaborationDependency` or `ndnsf.CollaborationRole`.
- Authority: harness dependency failure only. No Qwen model load, artifact
  generation, or CUDA loadability check occurred.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/qwen-stages/.spec160-qwen-stages-001.partial/`.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/qwen-stages-001/`.

The next replacement adds a source-bundled, prep-only `ndnsf.py` dataclass stub
for offline policy serialization. This does not authorize T004 to use the stub;
the real NDNSF binding remains required for live distributed inference.

## Stage Attempt 002: Qwen FP16 stage artifacts accepted

- Submission: `spec160-qwen-stages-002`.
- Run: `spec160-run-qwen-stages-002`.
- Source bundle SHA-256:
  `784f4fc0d1f6ef41ebfcadc0db0c3d7e1108bdbbd3a8c59c9c2305bb3a373916`.
- Slurm job: `173266`.
- Terminal state: `COMPLETED`, exit `0:0`, elapsed `00:01:53`.
- Accepted artifact directory:
  `/project/tma1/ndnsf-di/artifacts/spec160/qwen-stages/spec160-qwen-stages-002`.
- Reached:
  - Single RTX 5000 allocation.
  - 111-file source bundle copied into evidence.
  - Accepted SIF checksum verified.
  - Three FP16 `qwen-transformers` stage packages generated from the frozen
    Qwen model directory.
  - Each stage package loaded on CUDA with `require_cuda=True`.
- Layer coverage and digests:
  - stage 0 `/LLM/Pipeline/Stage/0`: `[0,8)`,
    SHA-256 `b844c54e1687a9c86e7fad42204fb28e056017e1ff46e910b8f2441eec022b7b`.
  - stage 1 `/LLM/Pipeline/Stage/1`: `[8,16)`,
    SHA-256 `b7f86958252c59368102602a9896c11494e0a8b5a5c97238a83ece34c71653d9`.
  - stage 2 `/LLM/Pipeline/Stage/2`: `[16,24)`,
    SHA-256 `82afae41d8017a0acca8e4edaafcc1142ce27e4510173313b407309d1712c2e6`.
- Policy SHA-256:
  `8316d23d6eb5941befc13549def89f58c3f7b193021c22a136a3d8aa5c59e768`.
- Runtime summary SHA-256:
  `35ab7a194382a0847e19b4d6d8c47ab0dd6c130251461eb1d1aae966a1aea3b3`.
- Evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/qwen-stages/spec160-qwen-stages-002/`.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/qwen-stages-002/`.

T003 is closed. The next gate was T004: execute a three-node secured NDNSF-DI
Qwen request with one role provider per node, using the accepted stage
artifacts and the real NDNSF binding. Live Attempts 001-005 showed that the
runtime path had drifted into a mixed SIF plus externally mounted replacement
libraries/bindings/dependencies. This is now treated as a blocking runtime
contract failure, not as Qwen execution evidence.

## Live Attempt 001: route and import failures

- Submission: `spec160-qwen-live-001`.
- Run: `spec160-run-qwen-live-001`.
- Source bundle SHA-256:
  `1e114b3eef53c94cee6f971427f035698c5799107b8c2837069e53c5bbe3bc6d`.
- Slurm job: `173267`.
- Terminal state: `FAILED`.
- Root causes:
  - Route setup ran `nfdc` before `NDN_CLIENT_TRANSPORT` was exported, so it
    attempted `/run/nfd/nfd.sock` instead of the job-local socket.
  - The runtime also exposed Python binding/import gaps for the NDNSF-DI app
    SDK path.
- Authority: harness/runtime startup failure only. No live Qwen request was
  executed.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/qwen-live-001/`.

## Live Attempt 002: manually canceled route-stage run

- Submission: `spec160-qwen-live-002`.
- Run: `spec160-run-qwen-live-002`.
- Source bundle SHA-256:
  `36eb53a757d178f21b3388d6957c2bdef1b870aee4bcd8171638ce091e1b5052`.
- Slurm job: `173268`.
- Terminal state: `CANCELLED by 64102`.
- Root cause: the run was manually canceled after route progress was mistaken
  for a hang. Later evidence showed route setup had succeeded before provider
  or user execution.
- Authority: canceled harness run only. It is preserved as a failed attempt,
  not as inference evidence.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/qwen-live-002/`.

## Live Attempt 003: Python binding version mismatch

- Submission: `spec160-qwen-live-003`.
- Run: `spec160-run-qwen-live-003`.
- Source bundle SHA-256:
  `28703ecb9c076d0465fa0075bfabe17a2f1ea5aba9ebab25ebdbb58a2d35ce39`.
- Slurm job: `173269`.
- Terminal state: `FAILED`.
- Reached: NFD startup and cross-node route setup succeeded.
- Root cause: the runtime Python was 3.10, but only the existing cp38
  `_ndnsf` extension had been uploaded; importing the real binding failed
  before provider/user execution.
- Authority: mixed runtime packaging failure only.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/qwen-live-003/`.

## Live Attempt 004: app SDK dependency/export failure

- Submission: `spec160-qwen-live-004`.
- Run: `spec160-run-qwen-live-004`.
- Source bundle SHA-256:
  `b462d49770bd8dd74be8410ba12d66ef23bf3c3e5d373720194641ce263b3f9b`.
- Slurm job: `173277`.
- Terminal state: `FAILED`.
- Reached: `node-0/controller.log` printed `SPEC160_CONTROLLER_READY`, proving
  the temporary py3.10 `ndnsf` binding loaded far enough for controller
  construction.
- Root cause: provider/user imports failed because the container lacked the
  matching app SDK dependency/export set: `cryptography` and py3.10
  `py_repoclient` were absent from the coherent runtime.
- Authority: mixed runtime dependency failure only. The correct repair is a
  coherent image rebuild, not ad-hoc mounted `vendor-site` and binding patches.

## Live Attempt 005: mixed-runtime native constructor crash

- Submission: `spec160-qwen-live-005`.
- Run: `spec160-run-qwen-live-005`.
- Source bundle SHA-256:
  `021d22943ecdf2928b811f2aaf3b8c0c2c7c5e881d52ac9ca12c23baf37a11e7`.
- Slurm job: `173284`.
- Terminal state: `CANCELLED by 64102` after provider crashes made success
  impossible; elapsed `00:04:25`.
- Runtime layout: accepted Spec 159 SIF plus externally mounted replacement
  `/source/native-libs/libndn-service-framework.so.0.1.0`, py3.10
  `_ndnsf`, py3.10 `_py_repoclient`, and ad-hoc `vendor-site` dependencies.
- Reached:
  - Three distinct nodes (`itiger07`, `itiger08`, `itiger09`) and GPUs.
  - Three NFD instances reached readiness.
  - TCP face creation, route installation, and multicast strategy setup
    succeeded on all three ranks.
  - Controller startup marker was written.
  - All three provider launch markers were written.
- Root cause observed:
  - Provider processes aborted before readiness and before Qwen model preload.
  - Scratch logs showed allocator corruption: rank 0 `double free or
    corruption (!prev)`, rank 1 `free(): invalid pointer`, and rank 2
    `munmap_chunk(): invalid pointer`.
  - `rank-step.err` recorded provider aborts for ranks 0 and 2.
- Authority: this is the decisive mixed-runtime failure. It is not Qwen model
  evidence, not CUDA placement evidence, and not distributed-inference logic
  evidence because execution failed during native provider construction.

## Runtime diagnostics after Attempt 005

- Import diagnostic job `173282`: with the mounted v6 source tree,
  `cryptography`, `py_repoclient`, `ndnsf._ndnsf`, app SDK modules,
  `provider`, and `user` imported successfully under Python 3.10. This proved
  imports alone were no longer the blocking failure.
- Fine-grained import trace job `173283`: all app SDK, provider, and user
  imports completed individually in under one second. This ruled out import-time
  model loading as the provider crash cause.
- Provider start trace job `173285`: each stage printed `TRACE import-ok`, then
  crashed inside `APPProvider.from_config()` before role selection, Qwen stage
  preload, or service registration. Python faulthandler showed the crash at
  `/source/pythonWrapper/ndnsf/service.py:874` inside
  `NativeServiceProvider(...)` construction.
- Runtime ldd diagnostic job `173286`: `_ndnsf` resolved
  `libndn-service-framework.so.0.1.0` from `/source/native-libs`, while lower
  dependencies (`libndn-cxx`, `libndn-svs`, `libnac-abe`, OpenABE, Boost,
  OpenSSL) resolved from the older SIF paths. This confirmed the live runtime
  was a mixed native stack.
- Python 3.10 constructor diagnostic job `173290`: `APPProvider.from_config()`
  again aborted with `double free or corruption (!prev)` at
  `NativeServiceProvider(...)` construction.
- Python 3.8 direct constructor diagnostic job `173292`: using the cp38
  `_ndnsf` binding and a generated trust schema, direct `ServiceProvider(...)`
  construction also aborted with `double free or corruption (!prev)` at the
  same native constructor. This rules out a py3.10-only ABI issue and points to
  the mixed native runtime contract itself.

T004 is blocked until T004b/T004c rebuild and validate one coherent runtime.
No further formal live Qwen attempt may mount replacement native libraries,
Python extension modules, or ad-hoc vendor-site dependencies over the accepted
Spec 159 SIF. The next acceptable gate is a clean runtime rebuild followed by
native constructor and single-node NDNSF-DI smoke validation inside Slurm.

## Clean Runtime Build 001: coherent local app-runtime image

- Build ID: `spec160-clean-runtime-20260727T215320Z`.
- Command:
  `packaging/ndnsf-di-container/oci/layered/scripts/build-layered-local.sh --target app --jobs 2 --app-build-id spec160-clean-runtime-20260727T215320Z --output results/spec160-itiger-multinode-qwen/spec160-clean-runtime-20260727T215320Z`.
- Build manifest:
  `results/spec160-itiger-multinode-qwen/spec160-clean-runtime-20260727T215320Z/build-manifest.json`.
- Terminal status: local Docker build `PASS`, duration `728.086` seconds.
- Image:
  `ndnsf-di:spec158-spec160-clean-runtime-20260727T215320Z`.
- Image ID:
  `sha256:16b99a0b82fb6ae4e98b1767cca3627eb1e322ed9a1493698666546caa5cbf69`.
- App source seal:
  `sha256:d6cbde0c2cbf992f4e74c2021f16c3b81f6cfd6978cde69ae24e7ccf72fd3e7c`.
- App lock digest:
  `sha256:a1670a77368a7b53783f43be95f7e2dc8f830da8925b4d7559c04217f2c5d51c`.
- Reused frozen lower layers:
  - ML lock:
    `sha256:97ad89dd24a9c8a7620a4e42965f82ab791e8258685c2c8c95b784ded4ea087a`.
  - NDN lock:
    `sha256:481e219da36223fba48fd0739b5e867f03be431d62e975c09b83fba4ea490653`.
  - NDN source seal:
    `sha256:d58aba2eba0d6b570044435d23df3bf307cfe3c86c26fbc63509e8465afd7a85`.
- Built app-layer components observed in the Docker log:
  - NDN-SVS compiled with the canonical Boost 1.71 compatibility patch and
    installed to `/opt/ndnsf-app`.
  - NDNSD compiled and installed to `/opt/ndnsf-app`.
  - NDNSF C++ library, `App_ServiceController`, and `di-native-provider`
    compiled and installed to `/opt/ndnsf-app`.
  - `ndnsf` pybind wheel, `py_repoclient` pybind wheel, and the
    NDNSF-DI Python owner packages were rebuilt inside the image.
- Runtime closure: Docker build step `verify-runtime-closure.py --root
  /opt/ndnsf-app` printed `RUNTIME_LIBRARY_CLOSURE_PASS:10`.
- Static runtime probe:
  `results/spec160-itiger-multinode-qwen/spec160-clean-runtime-20260727T215320Z/local-smoke/static-probe.json`
  reports `status: PASS`, `ndnsf: present`, `ndnsf_distributed_inference:
  present`, `onnxruntime: 1.20.1`, `torch: 2.6.0+cu124`, and `transformers:
  4.48.2`.
- Local linkage check: `_ndnsf` resolves
  `libndn-service-framework.so.0.1.0`, `libndnsd.so.0.1.0`, and
  `libndn-svs.so.0.1.0` from `/opt/ndnsf-app/lib`, while `libndn-cxx` and
  `libnac-abe` resolve from `/opt/ndn-base/lib`. No `/source/native-libs`,
  mounted pybind extension, or `vendor-site` override is used.
- Local provider constructor smoke:
  `results/spec160-itiger-multinode-qwen/spec160-clean-runtime-20260727T215320Z/local-smoke/provider-python-api-constructor.log`
  imports the Python API and reaches the native provider constructor without
  allocator abort. It terminates as a normal configuration exception:
  `RuntimeError Cannot set non-existing identity
  /NDNSF-DistributeInference/example/provider/provider as default`.
- Authority: T004b local coherent runtime image build is complete. This is not
  yet a Slurm runtime smoke, SIF materialization, GPU CUDA acceptance, or
  three-node Qwen inference result. T004c remains the next blocking gate.

## Materialization Attempt 001: GHCR digest pull auth failure

- Submission: `spec160-submission-sif-178122a30d0a-001`.
- Run: `spec160-run-sif-178122a30d0a-001`.
- Slurm job: `173293`.
- OCI digest:
  `ghcr.io/matianxing1992/ndnsf-di@sha256:178122a30d0a29bac9ff1c9759667cffa9a7e9f6a969daefa5187726acc0c6f7`.
- Terminal state: `FAILED`, exit `127:0`, elapsed `00:00:00`.
- Root cause: `apptainer build docker://...` on the compute node failed with
  `unable to retrieve auth token: invalid username/password: unauthorized`.
- Authority: registry-auth materialization failure only. It is not runtime,
  SIF, CUDA, or Qwen evidence.
- Durable evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/materialization/.spec160-submission-sif-178122a30d0a-001.partial/`.

## Archive Materialization Attempt 001: clean runtime SIF

- Submission: `spec160-submission-sif-archive-178122a30d0a-001`.
- Run: `spec160-run-sif-archive-178122a30d0a-001`.
- Replaces failed job: `173293`.
- Slurm job: `173294`.
- Terminal state: `COMPLETED`, exit `0:0`, elapsed `00:11:38`.
- Input archive:
  `/project/tma1/ndnsf-di/images/spec160-178122a30d0a/candidate-docker-archive.tar.gz`.
- Archive SHA-256:
  `90fc608aa2eb3710b353891196c13c484af55363ef248ad52d6b578fd3982377`.
- Source OCI digest:
  `ghcr.io/matianxing1992/ndnsf-di@sha256:178122a30d0a29bac9ff1c9759667cffa9a7e9f6a969daefa5187726acc0c6f7`.
- Local image ID:
  `sha256:16b99a0b82fb6ae4e98b1767cca3627eb1e322ed9a1493698666546caa5cbf69`.
- Promoted SIF:
  `/project/tma1/ndnsf-di/releases/spec160-178122a30d0a-archive/runtime.sif`.
- SIF SHA-256:
  `8d8d95e7a5de86036ad62d972b911a66f2160553c6b3295d029dd3e7ae26106f`.
- Static probe: `status: PASS`, with `ndnsf`, `ndnsf_distributed_inference`,
  `onnxruntime 1.20.1`, `torch 2.6.0+cu124`, and `transformers 4.48.2`.
- Durable evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/materialization/spec160-submission-sif-archive-178122a30d0a-001/`.
- Local mirror:
  `results/spec160-itiger-multinode-qwen/materialization-archive-178122a30d0a-001/`.

## Runtime Smoke Attempts 001-003

- Runtime Smoke Attempt 001:
  - Submission: `spec160-submission-runtime-smoke-178122a30d0a-001`.
  - Slurm job: `173295`.
  - Terminal state: `FAILED`, exit `1:0`, elapsed `00:00:29`.
  - Reached: job-local NFD, ServiceController construction, ServiceProvider
    construction, DKEY success, `/HELLO` service registration, and provider
    permission installation.
  - Root cause: the smoke harness waited for obsolete provider-ready text
    (`Provider .* registered service /HELLO`) while the current runtime logs
    `registered service prefix=/HELLO` and `Registered service handler for
    /HELLO`. The user request was never launched.
  - Authority: harness wait-condition failure only. It is not a runtime crash.
- Runtime Smoke Attempt 002:
  - Submission: `spec160-submission-runtime-smoke-178122a30d0a-002`.
  - Replaces failed job: `173295`.
  - Slurm job: `173296`.
  - Terminal state: `COMPLETED`, exit `0:0`, elapsed `00:00:12`.
  - Reached: one `/HELLO` request completed and dependency imports passed.
  - Evidence limit: the generated summary still used the obsolete provider-ready
    string and reported `providerReady=false`, so this pass is preserved but not
    used as the clean T004c closure artifact.
- Runtime Smoke Attempt 003:
  - Submission: `spec160-submission-runtime-smoke-178122a30d0a-003`.
  - Replaces ambiguous job: `173296`.
  - Slurm job: `173297`.
  - Terminal state: `COMPLETED`, exit `0:0`, elapsed `00:00:12`.
  - Verified SIF SHA-256:
    `8d8d95e7a5de86036ad62d972b911a66f2160553c6b3295d029dd3e7ae26106f`.
  - Runtime summary:
    `status=PASS`, `controllerReady=true`, `providerReady=true`,
    `requestOk=true`, `allocatorCorruption=false`.
  - Imports: `ndnsf`, `py_repoclient`, `ndnsf_distributed_inference`,
    `onnxruntime 1.20.1`, `torch 2.6.0+cu124`, and `transformers 4.48.2`.
  - Durable evidence:
    `/project/tma1/ndnsf-di/evidence/spec160/runtime-smoke/spec160-submission-runtime-smoke-178122a30d0a-003/`.
  - Local mirror:
    `results/spec160-itiger-multinode-qwen/runtime-smoke-178122a30d0a-003/`.
- Authority: T004c is complete. The clean SIF has passed Slurm runtime smoke
  without mounted replacement native libraries, Python extension modules, or
  ad-hoc `vendor-site` overrides. This is still not three-node Qwen inference
  evidence; T004d remains the next blocking gate.

## Live Attempt 014: operation-status binding failure

- Submission: `spec160-qwen-live-014`; Slurm job `173319`.
- Terminal state: `FAILED`, exit `1:0`, elapsed `00:10:32`.
- Immutable evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/qwen-live/.spec160-qwen-live-014.partial/`.
- The secured request reached all three Qwen CUDA providers. Authentication,
  discovery, ACK collection, selection, and handler dispatch completed.
- All three roles then failed at `python/ndnsf/service.py:1030` with
  `TypeError: Object of type 'str' is not an instance of 'none'`. The requester
  timed out after `180513.514 ms`; this is not accepted inference evidence.
- Root cause: `PyCollaborationContext::reportOperationStatus()` used a
  conditional expression combining a dictionary accessor with `py::none()`.
  The common expression type attempted to construct `py::none` from present
  string fields.
- Local repair: use explicit missing/`None` branches before casting text and
  integer fields in `pythonWrapper/src/ndnsf/_ndnsf.cpp`.
- Local verification: Waf build passed; Python native extension rebuilt; 12
  related Python tests and 3 `GenericDynamicApi/CollaborationStatus` C++ tests
  passed.
- Replacement image status: built locally as
  `ndnsf-di:spec158-spec160-operation-status-binding-20260728T052052Z-e`,
  image ID
  `sha256:af0c3a3ce357f9fb5977dd1d3bad0b71d5ae2237b6a23675eccc6f4df476fe3b`.
  The build manifest is
  `results/spec160-itiger-multinode-qwen/spec160-operation-status-binding-20260728T052052Z-e/build-manifest.json`.
- Authority: Attempt 014 is immutable negative evidence. No live Attempt 015
  has been submitted.

## Local Docker operation-status binding gate

- Terminal status: `PASS`.
- Evidence:
  `results/spec160-itiger-multinode-qwen/local-docker-operation-status-20260728T070417Z/`.
- Scope: one container with NFD, one `ServiceController`, one provider, and one
  user; Docker limits were `--memory=4g --memory-swap=5g --cpus=2`.
- Workload: one `/AI/LLM/Pipeline/Fake` collaboration assignment, one stage,
  one measured request, and a 227-byte fake response. The provider and user
  both used `runtime=fake`; no Torch/Qwen model or three-node inference was
  started.
- Binding coverage: the DI provider wrapper invoked
  `_report_preparation()`, which constructs `ServiceOperationStatus` and calls
  `CollaborationContext.report_operation_status()`. Provider evidence contains
  signed selection-status states `1` through `4` for the same assignment and
  the final stage marker. The user CSV records `status=ok` at `64.319 ms`.
- Regression assertion: the provider log contains no
  `Object of type 'str' is not an instance of 'none'`.
- Evidence hygiene: checksums pass, and the harness removed bootstrap tokens,
  temporary keychain material, and requester app state before preserving the
  result.
- Authority: this closes the local binding/runtime gate only. It does not
  replace T004d three-node Qwen/CUDA evidence and does not authorize an
  automatic live Attempt 015.

## iTiger SIF operation-status binding gate

- Archive:
  `/project/tma1/ndnsf-di/images/spec160-af0c3a3ce357-v1/candidate-docker-archive.tar.gz`,
  SHA-256
  `65074a6fea4c2f85f32f6fbe24c70b3529167a5cb1c582d55026b7ef88e8028f`.
  The remote copy passed both SHA-256 verification and `gzip -t`.
- Materialization: Slurm job `174124` completed `0:0` in `00:12:47` on
  `itiger04`. It promoted
  `/project/tma1/ndnsf-di/releases/spec160-af0c3a3ce357-operation-status/runtime.sif`,
  SHA-256
  `6638c813016e151fd6c19825db8dc01e3a326c23716084677eee3c4da935d52e`.
  Durable evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/materialization/spec160-submission-sif-operation-status-af0c3a3ce357-001/`.
- Materialization evidence caveat: the executed runner wrote its checksum
  manifest with the pre-promotion `.partial` absolute path and included a
  checksum self-reference. The ten non-self-referential entries were verified
  after mapping that path to the final directory, and the durable SIF was
  independently hashed. The runner is fixed for future submissions; the
  executed manifest remains unchanged as evidence.
- Smoke Attempt 001: Slurm job `174164` is preserved as FAIL, exit `127:0`,
  elapsed `00:00:04`. The compute node lacked `/usr/bin/time`; Apptainer and
  NDNSF were not started.
- Smoke Attempt 002: Slurm job `174170` is preserved as FAIL, exit `1:0`,
  elapsed `00:00:11`. Controller and Provider became ready, but the user
  runtime journal was incorrectly placed on project NFS and `flock()` failed
  with `EBADF`; no request was submitted.
- Smoke Attempt 003: Slurm job `174176` completed `0:0` in `00:00:11` on
  `itiger04`, explicitly replacing job `174170`. The same SIF ran one
  Apptainer container with NFD, Controller, one Provider, one User, a
  job-local runtime journal, one fake stage, and one measured request under
  Slurm `ReqMem=5G` with no GPU allocation or model mount.
- Attempt 003 acceptance: the user recorded `status=ok` at `50.376 ms`; the
  Provider recorded selection-status states `1` through `4` and
  `LLM_PIPELINE_STAGE_FINAL`. Checksums pass. Logs contain no Qwen, Torch, CUDA,
  old `str`-to-`none` binding error, or runtime-journal error. Bootstrap
  tokens, temporary keychain material, and app state were not retained.
- Remote evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/operation-status-sif-smoke/spec160-submission-operation-status-sif-smoke-af0c3a3ce357-003/`.
  Local mirror:
  `results/spec160-itiger-multinode-qwen/operation-status-sif-smoke-af0c3a3ce357-003-pass/`.
- Authority: the repaired candidate has passed both local Docker and iTiger
  single-node SIF operation-status gates. This still is not T004d Qwen/CUDA or
  three-node inference evidence.

## Live Attempt 015: runtime success, analysis-gate failure

- Submission: `spec160-qwen-live-015`; run `spec160-run-qwen-live-015`;
  Slurm job `174221`, explicitly replacing failed job `173319`.
- Terminal state: `FAILED`, exit `1:0`, elapsed `00:07:07`. The first result
  is preserved and was not retried.
- Candidate SIF SHA-256:
  `6638c813016e151fd6c19825db8dc01e3a326c23716084677eee3c4da935d52e`.
  Source bundle SHA-256:
  `ec053168605107dbb71a2bbb6e2ffb22eca6d4aadfde51f92ddc9ea4869ab3eb`.
- Physical placement: stages 0, 1, and 2 ran respectively on `itiger07`,
  `itiger08`, and `itiger09`, using three distinct NVIDIA RTX 5000 Ada GPU
  UUIDs. Every stage recorded `device=cuda:0` and `cpuFallback=0`.
- Dependency evidence: both segmented cross-node fetches completed. Stage 0
  output and stage 1 input shared SHA-256
  `784e075ed8db8a28ac108ae0849353f1a990afb164b1938c1b0a73fce0a134bf`;
  stage 1 output and stage 2 input shared SHA-256
  `d038b303945a672acadf6f7c04df9fc13c8c194817ed14e2b865cdceffa2170f`.
  Their planned Data names are distinct.
- Requester result: one measured request returned `status=ok` in
  `2794.666 ms`. Expected and actual top token were `27024`; returned logits
  shape was `[1, 5, 151936]`, with layer ranges `[0,8)`, `[8,16)`, and
  `[16,24)`.
- Failure boundary: after all ranks completed, the evidence analyzer required
  each internal request ID to equal `spec160-qwen-live-015`. The application
  correctly emitted `spec160-qwen-live-015-0` for measured request index zero,
  so this post-run assertion returned exit 1. The locally corrected assertion
  passes read-only against the immutable evidence.
- Evidence hygiene: the original 58 checksum rows pass from the evidence root;
  no `bootstrap-tokens.txt` or private key was retained; registered allocator,
  CPU-fallback, CUDA-unavailable, and old pybind failure markers are absent.
- Remote immutable evidence:
  `/project/tma1/ndnsf-di/evidence/spec160/qwen-live/.spec160-qwen-live-015.partial/`.
  Local mirror:
  `results/spec160-itiger-multinode-qwen/qwen-live-015-analysis-gate-fail/`.
- Authority: Attempt 015 provides positive runtime evidence for all three
  technical T004d conditions, while its formal terminal result remains FAIL.
  The user chose the no-rerun evidence-acceptance path after this boundary was
  reported twice. T004d closes from the immutable workload evidence; job
  `174221` is not relabeled, its partial directory is not promoted, and no
  Attempt 016 was submitted.
