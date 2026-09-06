# Tiger Functional Profile Contract v1

**Spec180 revision 112 scope**: this contract defines one YOLO functional
profile. Qwen and distinct-GPU qualification are deferred.

## Common fixed runtime

- Apptainer: `/opt/apptainer/1.5.3/bin/apptainer`, version `1.5.3`
- Invocation: Slurm uses `--export=NONE` plus the explicit allow-listed
  `SPEC180_*` assignments; the job wrapper then invokes `exec --nv --cleanenv
  --pwd /bundle` inside the fixed SIF
- Container working directory: `/bundle`
- Image owner: local Apptainer build; Tiger never builds or overlays the image
- Model owner: external read-only content-addressed storage staged to node-local
  scratch after hash verification
- Identity owner: isolated HOME/PIB/TPM per process
- Runtime: ONNX Runtime; model roles require CUDA execution
- Device binding: each model Provider is launched with explicit
  `CUDA_VISIBLE_DEVICES=0` and must report its role, PID, ONNX Runtime CUDA
  provider, and matching physical UUID. The same UUID is expected for the
  three model roles in this one-GPU profile. The CPU Merge Provider has no
  visible CUDA device.
- Network: host NFD processes and explicit checked-in routes; no MiniNDN on Tiger
- Protocol timing: 5000 ms initial SVS settle, 1500 ms ACK timeout, 60000 ms
  request timeout; ACK closure is timeout-driven with no predeclared role
  coverage predicate; no profile-local override
- Input security: registered YOLO input is an encrypted repository reference;
  input/result plaintext and secret capabilities are forbidden in logs/evidence
- Logging: checked-in bounded `NDN_LOG` filter plus structured lifecycle/runtime
  evidence files
- Cleanup: bounded termination, every child status collected, run-owned scratch
  removed, shared content-addressed model cache retained only by policy

The only public entry is
`packaging/ndnsf-di-container/jobs/spec180/submit.sh <gate> <profile> <run-record>`.
Direct `.sbatch`, ambient exported configuration, or a changed working directory
is invalid.

The three positional arguments are canonical repository-relative or absolute
paths as follows: `gate` is exactly `yolo-functional`;
`profile` is the checked-in rendered-profile JSON consumed by C2; and
`run-record` is a candidate-bound JSON record containing only the deltas listed
below plus its candidate, workload, and output identities. The wrapper MUST
reject missing, extra, duplicate, or unknown fields, canonicalize and hash both
JSON inputs, and compare the rendered Slurm argv/environment with the profile
before invoking `sbatch`. A `--dry-run`/render mode may produce a read-only
closure report, but the production entrypoint MUST be the same wrapper and MUST
not accept direct `.sbatch` arguments or ambient overrides.

## Gate profiles

### `yolo-functional`

This profile launches four independent Provider processes on one node and one
GPU.

```text
nodes=1
ntasks=1
gres=gpu:rtx_6000:1
mem=32G
time=00:20:00
providerCount=4
modelProviderCount=3
mergeProviderCount=1
gpuProviderDevices=0,0,0
mergeCudaVisibleDevices=unset
requestCount=1
ackTimeoutMs=1500
requestTimeoutMs=60000
initialSvsSettleMs=5000
inputMode=REPO_REF
decodePolicy=not-applicable
```

## Allowed run-record deltas

Only candidate ID, exact local/remote SIF paths and hashes, run/output IDs,
external model root/manifest, and registered workload manifest may differ
between invocations. Provider/GPU counts, node/memory/time envelope, roles,
timeouts, routes, identity set, working directory, runtime, logging, and cleanup
are profile-owned and immutable.

There is no `qwen-functional` gate and no second warm request in Spec180.

The renderer must reject missing, extra, unconsumed, contradictory, or ambient
values before any upload, SSH mutation, staging, or Slurm call.
