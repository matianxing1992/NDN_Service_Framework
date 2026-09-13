# Spec186 baseline and host inventory

**Captured:** 2026-09-12 (America/Chicago)
**Branch:** `SPEC184Experiments`
**Purpose:** freeze the source and external inputs before any Spec186 candidate is prepared.

## Source seal

| Field | Value |
| --- | --- |
| Spec186 checkout HEAD | `309f237915b661a1264487fbc58c6249e6e84dd7` (`docs(spec186): record native artifact cleanup`) |
| Required source baseline | `575b43cc93bbed29932303caf3d09974f1585af7` |
| Required baseline parent | `0df480f4a0979b697eee820038266eb813ba7c62` |
| Required baseline tree | `f267e047b8ee2d39ba521692e9ec03756af326a9` |
| Baseline subject | `Record local experiment prepare smoke` |
| Object verification | `git cat-file -t` returned `commit`; ancestor check passed |
| Checkout status | no tracked changes; private `Experiments/TigerCluster/.keys/` remains untracked and is excluded |

The baseline object is an ancestor of the Spec186 documentation commit. The
Spec186 implementation must bind its candidate manifest to the exact baseline
SHA and tree; the older Tiger handoff lock is not accepted as this baseline's
source seal.

## Local experiment host

| Capability | Observation | Spec186 meaning |
| --- | --- | --- |
| OS/kernel | Ubuntu 20.04 host, Linux `5.15.0-139-generic`, `x86_64` | local CPU/MiniNDN preparation host |
| CPU/RAM | 6 logical CPUs; `MemTotal=10,179,596 kB`; `MemAvailable=6,800,852 kB` | keep build and local runs bounded; max build parallelism `-j4` |
| C++ toolchain | `g++ 9.4.0` (`/usr/bin/g++`) | record in candidate; rebuild if toolchain changes |
| Python | `Python 3.8.10` | existing Python orchestration entrypoints can be inspected/run |
| Apptainer | `/usr/local/bin/apptainer`, version `1.5.3` (local install switched on 2026-09-12) | local SIF inspection/build capability aligned with Tiger compute; no claim of Tiger execution |
| MiniNDN | `minindn` command not installed | real local MiniNDN run is currently blocked until the project runner/dependency is supplied |
| NDN tools | `/usr/local/bin/ndnsec`, `/usr/local/bin/nfdc`; `nfd` path not found in command lookup | identity tooling exists; complete local topology still needs runtime closure |
| Waf | `waf` command not installed in PATH | use repository wrapper/build tree only after T005 convergence |
| SSH/rsync | `/usr/bin/ssh`, `/usr/bin/rsync` | transport tools present; no Tiger endpoint/account supplied in this host inventory |
| Slurm | `sbatch` not installed | Tiger allocation/dispatch cannot run on this host |
| NVIDIA | `nvidia-smi` not installed | no local GPU or CUDA observation is possible |

## Model inputs

| Input | Path | Size | SHA-256 | Status |
| --- | --- | ---: | --- | --- |
| YOLOv8n ONNX | `/home/tianxing/NDN/QMUAS_Drone/DetectionModule/yolov8n.onnx` | 12,823,490 bytes | `4d2f387218a088c40a19dd8b41067ba729504f37d258f133377221b148e2f373` | available for candidate preparation |
| YOLO military ONNX | `/home/tianxing/NDN/QMUAS_Drone/DetectionModule/yolov8n_military.onnx` | 12,241,279 bytes | `22eda21fecbba019c487f27e5280c591b0801733257df96e5c634ba4347192b4` | available; select explicitly in profile |
| Qwen2.5 0.5B GGUF | history checkout `third_party/qwen/qwen2.5-0.5b-instruct-q4_k_m.gguf` | 491,400,032 bytes | `74a4da8c9fdbcd15bd1f6d01d621410d31c6fc00986f5eb687824e7b93d7a9db` | not a Qwen3-0.6B substitute |
| Qwen3-0.6B weights/tokenizer | targeted local paths | — | — | `WAITING_EXTERNAL_INPUT`: no compatible artifact found |

The Qwen2.5 GGUF is retained only as a negative inventory result. It must not
be presented as Qwen3-0.6B evidence. Tiger GPU, Slurm account/partition,
node/GPU UUID, project storage and a compatible Qwen3-0.6B artifact remain
external inputs.

## Reproduction commands

```bash
git rev-parse HEAD
git show -s --format='%H%n%P%n%s' 575b43cc93bbed29932303caf3d09974f1585af7
git rev-parse 575b43cc93bbed29932303caf3d09974f1585af7^{tree}
git merge-base --is-ancestor 575b43cc93bbed29932303caf3d09974f1585af7 HEAD
uname -a; python3 --version; g++ --version | head -1; apptainer --version
command -v minindn nfd ndnsec nfdc waf ssh rsync
nvidia-smi --query-gpu=name,uuid,memory.total --format=csv,noheader
sbatch --version
```

The final two commands return command-not-found on this host; their failures
are capability evidence, not experiment results.

## Tiger compute preflight

The local runtime switch is recorded in
[`local-apptainer-20260912.md`](local-apptainer-20260912.md). The external capability probe is recorded in
[`tiger-preflight-20260912.md`](tiger-preflight-20260912.md). It allocated
`itiger05` with an RTX 6000 Ada (48 GiB, driver `560.28.03`) and verified
Slurm/project storage plus compute-node Apptainer `1.5.3-1.el9` through
`apptainer --version`. The login node reports Apptainer `1.3.4-1.el9`; the
compute-node `apptainer version` subcommand hangs under a five-second bound and
is retained as a boundary. Login-node SIF execution is outside the workflow;
only the allocated compute-node runtime is used for Tiger jobs. This is
resource readiness only; no candidate was staged or executed.
