# Qwen physical GPU experiments and strict CPU reference

## Freeze the comparison

- Bind the exact weights, tokenizer, input token arrays, greedy policy, output
  budget, graph variant, runtime, role layout, checkpoints and hardware. For
  the Spec 235 strict comparison, both cold and warm produce 32/32 exact tokens
  against the frozen independent unsplit CPU reference. Also rerun the native
  unsplit CPU oracle on the admitted effective graph, bound to the actual
  deployment. Capture actual CUDA compute on both Provider nodes, authenticated
  state progression, actual warm reuse and nonforced cleanup.
- For shared native or runtime changes, qualify the affected installed CPU
  checks and pass same-SIF 0.6B CUDA cold/warm before spending 27B GPU time.
  An operator, dtype or runtime incompatibility can justify a distinct graph
  variant. Verify that its weights, public ABI and state ownership are preserved;
  qualify the exact variant independently. A native capability rejection makes
  TP unsupported for that candidate. GPU presence alone does not establish TP.
- Keep source, assembled artifacts, resident Session and authenticated
  conversation state as separate measurements. A warm round uses the same
  signed role/device/artifact runner with its committed parent. Verify zero
  prefix recomputation, coverage of all required state ports and correct
  progression. Only claim GPU state-value agreement if the oracle compared it.

## Diagnose the actual boundary

- The 27B CUDA GQA graph needed host-side INT32 length metadata and layer-owned
  shape/position subgraphs. Check the type, provider assignment and consumers
  before cutting; host shape values crossing the boundary used by model
  computation can make native admission fail. Validate the exact cut and
  completeness of KV, convolution and recurrent state mapping independently.
- A retained model consumes device memory, so a legitimately pinned continuation
  needs its additional working set and live signed free memory, with full-role
  device capacity checked separately. For a cold round longer than a short
  placement cache TTL, confirm the valid authenticated fixed-role lease owns
  its original placement. Keep exact role/runner/artifact/device binding and
  failure closure. Test expiry of the opportunistic cache independently of grant
  expiry before an expensive retry. In Tiger job `306364`, warm planning
  retained the pinned placement 704.779 seconds after the last cold cache
  install, beyond the cache's 600-second TTL.
- When a Provider emits the last token and waits for checkpoint COMMIT, trace
  receipt publication, requester verification and the actual commit separately.
  In Spec 235's first 1:2 run, the 86-state role's receipt approached the signed
  SVS packet limit before encapsulation. Segmentation conflicts with the
  producer-bound collaboration packet subscription. The installed codec test
  reduced 86 state digests from 7,988 to 5,297 bytes of payload, preserving all
  digests and the canonical commitment. Keep the same signer/name admission
  checks and verify the whole asymmetric cold/warm workload.
- In the 27B FP16 case, warm token 24 diverged. The same warm activation on the
  same GPU type reproduced the CUDA full-vocabulary projection's ranking change.
  Disallowing reduced-precision GEMM reduction restored the CPU result in that
  one-op probe. An explicit FP32 projection rounded back to the original FP16
  output reproduced it as a separate diagnosis. Only full cold/warm GPU and CPU
  runs qualified the mitigation. The accepted Spec 235 experiment sealed
  `ORT_CUDA_GEMM_OPTIONS=2` in an ORT 1.31.1 image. ORT describes it as a testing
  control, so bound that result to the exact experiment candidate. Keep CPU
  strict and treat same-prefix CUDA comparisons as diagnosis.

## Validate and report

For the installed native model oracle, use the frozen configuration and
allocation evidence, matching owner, original boot/config/release digests and
a Slurm allocation on either physical node that the configuration maps to the
selected role. A healthy worker can run the CPU oracle if the control host
cannot allocate sufficient memory. Where supported, the host launcher is:

```bash
"$control_python" -I -m ndnsf_distributed_inference.ops.container \
  --deployment "$configuration" --boot-id "$boot_id" \
  --configuration-sha256 "$config_sha" --release-sha256 "$release_sha" \
  --management oracle --node "$node_id" --timeout "$oracle_budget" \
  --model-reference --require-fixed-role-conversation 32 32
```

Use the actual checkpoint expiry and observed mounting/staging times to plan
oracle start. An independent live signature check preserves evidence before
expiry. The native full reference still performs its own authentication and CPU
generation. Keep inference complete and oracle pending distinct until both
pass. Once baseline passes, compare matched layout samples under the same
candidate, workload and hardware conditions.

For each sample tie records to run/request/role/epoch and report cold/warm,
TTFT from the requester's monotonic clock, decode throughput with its token
denominator, per-role phases, logical payload, encoded TensorBundle and NDN wire
bytes separately. Bind Slurm peaks to the actor step ID. Label sampled GPU memory
as sampled, and leave unmeasured disk occupancy or instantaneous peaks
unavailable. Include failed attempts in the layout denominator.

Tiger 27B job `306277` on two RTX 6000 Ada devices
completed exact 32/32 cold and warm, with live checkpoint job `306256` and
CPU whole-graph oracle job `306294`. The oracle generated 128 reference state
digests per round and verified progression and cleanup. It reported
`providerStateValuesCompared=false`. Record fresh job and artifact identities
for each new experiment.
