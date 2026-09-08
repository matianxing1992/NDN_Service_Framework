# User-directed C++ NDN/SIF diagnostic

2026-09-08. The user explicitly requested a simple multi-node C++ NDN example
inside the existing SIF before continuing the YOLO qualification work. This
authorizes a bounded, CPU-only, two-node Slurm transport diagnostic before the
remaining YOLO gates. It does not promote an image or satisfy YOLO acceptance.

Scope: one C++ ndn-cxx producer and consumer, three distinct Interest names,
exact run-token payloads and verified DigestSha256 Data. One NFD per node with
CS disabled, a private Unix socket/HOME, and a TCP face/route between nodes.
Digest verification checks Data integrity, not producer identity or NAC-ABE.

Owners: apps/ndn-smoke.cpp, tools/build-ndn-smoke.sh, tools/ndn_smoke.py and
jobs/ndn-smoke/run.sbatch under Experiments/TigerCluster. Reuse baseline.py's
NFD config and process owner. Build the small binary locally with system g++,
load it against the SIF's ndn-cxx 0.9.0/Boost 1.71 libraries, bind the read-only
probe bundle, and execute that same binary on both allocated nodes.

Existing image: historical spec180-runtime SIF; target retained SHA256
b6710fd696a7f962f67f67a54278d92a15babb856c4af428a3f04ba54dc83285 at
/project/tma1/ndnsf-di/candidates/spec180-runtime-b6710fd6/spec180-runtime.sif.
This is not the unfinished Spec183 final image. Local cached bytes have an
unresolved read-integrity history: local execution is runtime-only evidence;
both Tiger ranks must separately verify the retained pinned hash.

Retain artifacts in results/ndn-smoke-20260908 and the corresponding project
storage run directory. No image build, model, GPU, broad MiniNDN campaign or
unrelated unit suite is required for this diagnostic.

Current observations before cluster submission:
- Host build initially selected Linuxbrew ld and failed transitive linking;
  explicit system PATH/g++/ld fixed it without rebuilding framework libraries.
- SIF ldd resolves the binary's ndn-cxx and Boost libraries; --help executes.
- First launcher rejected Apptainer's optional fuse2fs warning as a missing DSO;
  ldd detection now requires `=> not found`. No NFD started in that failed run.
- local-v2 transferred all three actual Data packets, but the C++ deadline fired
  after completion and a stale success flag emitted PASS. That run is not accepted
  as a clean pass. Stop the io_context on completion, clear success on failure,
  and reject any FAIL line; repeat only this affected small transport case.

## Verified runtime results

| Run | Actual execution | Verdict and reuse |
| --- | --- | --- |
| local-v3 | Two private NFDs on this host inside the cached SIF; three matching packets; both apps exit 0; children reaped and scratch removed | PASS for local transport only; image identity remains unqualified |
| 209980 / ndn-smoke-20260908a | itiger01 producer and itiger02 consumer both transferred all three packets; both verified the pinned SIF hash | Job FAIL 1:0, 61 seconds: producer-side wrapper timed out waiting for consumer-done.json, despite the actual application successes. Preserve as a failed orchestration run |
| 209981 / ndn-smoke-20260908b | Same two physical nodes, same binary and pinned SIF; corrected local producer completion check; three exact Data names/tokens and valid digests | PASS; allocation, batch and workload step COMPLETED 0:0; 45 seconds; clean child exits/reaping and removed scratch on both ranks |

209980 demonstrates that peer-file visibility was an unnecessary completion
dependency. Delayed shared-filesystem visibility is a possible explanation for
the missed marker; filesystem caching itself was not instrumented. The fix waits
for the owned producer process and validates its log, then relies on srun and the
parent's two-rank result aggregation. The one affected repeat was 209981; neither
the binary nor SIF was rebuilt or transferred again.

Both final ranks observed Apptainer `1.5.3-1.el9`, resolved the binary's dynamic
libraries, verified SIF SHA256 `b6710fd696a7f962f67f67a54278d92a15babb856c4af428a3f04ba54dc83285`
and executed binary SHA256 `83d4538ec744f01dbd44dab35fdf9334fc7ba3083df0a00cc04152601ce6c01e`.
The final launcher SHA256 is `5a399775856dec130b11888ddb1e657986dfe22ae14d396e806df8f955a39cb8`.
Nodes used TCP `172.16.0.11:29680` and `172.16.0.12:29680`, private Unix sockets
and keychains. No GPU was requested. The same historical SIF was executed with
the probe binary mounted read-only; it is not a newly packaged Spec183 image.

Durable raw evidence:
- Local: `Experiments/TigerCluster/results/ndn-smoke-20260908/`, including
  `local-v3/`, `collected-209980/`, `collected-209981/`, remote script audits and
  `slurm-accounting.txt`.
- Tiger: `/project/tma1/ndnsf-di/results/ndn-smoke-20260908a/` and
  `/project/tma1/ndnsf-di/results/ndn-smoke-20260908b/`.
- Each collected directory retains the exact manifest/bundle, per-rank SIF
  observations, NFD config/logs, routes/faces, application logs and cleanup results.
  Final retained reanalysis verified all four bundle hashes, distinct hosts,
  three packets per app, no FAIL lines and clean cleanup. It did not rerun NDN.

Reuse this configuration for the next minimal NDNSF service exercise, then the
remaining YOLO gates. This does not invalidate the user's delivered MiniNDN PASS
on another machine; it does not establish or refute NDNSF authorization, MiniNDN,
YOLO numerical correctness or CUDA. Those retain their own evidence boundaries.

Workflow: Context Mode project health passed and CodeGraph exploration was used;
Spec Kit plan/tasks carry the diagnostic scope. GSD health has no errors but
reports pre-existing worktree/noncanonical-handoff warnings; preserve them and
use repository-backed checkpoints without deleting another task's state. ARS is
not applicable to this three-packet operational diagnostic.
