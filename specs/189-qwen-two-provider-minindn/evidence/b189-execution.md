# B189-3 Execution Evidence

**Status**: IN_PROGRESS / NOT_NATIVE_PASS

### Attempt r01 — candidate identity preflight (2026-09-18)

The two-provider runner was invoked with the frozen two-stage candidate, but it
stopped before MiniNDN startup at `PROVIDER_BINARY_DIGEST_MISMATCH`. The
command-line expected digest had one extra character; the run therefore did
not start Controller, Authority, either Provider, or a requester. Raw launcher
log: `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r01/launcher.log`
(SHA-256 `b1fd445096e5f0786ffe9c189cfabe32a2077871f471176d5ba9994e19e7e38e`).
This is a harness identity-boundary failure, not a product or protocol result.

### Attempt r02 — immutable source preflight (2026-09-18)

After correcting the provider digest, the runner stopped before MiniNDN startup
at `MODEL_CANONICAL_SOURCE_NOT_IMMUTABLE`. The canonical graph and external
initializer were still writable (`0664`), so the runner refused to hard-link
them into the run directory. Raw launcher log:
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r02/launcher.log`
(SHA-256 `19b3d017526dc7c7630ff240b2657d210581f4c79254073956efc399bd14441a`).
No native process was started.

### Attempt r03 — build receipt preflight (2026-09-18)

With the candidate files read-only, the runner materialized the requester
configuration and immutable canonical objects, then stopped at
`BUILD_RECEIPT_DIGEST_REQUIRED` in its final identity fence. No MiniNDN
process was started. Raw launcher log:
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r03/launcher.log`
(SHA-256 `4450117cec97109bb11480e9145bcebe6621250a5e8f244db3172bc70fd716d9`).
The current receipt digest is recorded before the next retry.

### Attempt r04 — native preparation state-boundary failure (2026-09-18)

This attempt passed the candidate identity fence and started a real MiniNDN
topology. Controller, Authority, Provider-0 and Provider-1 all reached their
ready markers and both providers emitted signed `DI_PLACEMENT_V3_OFFER`
decisions for their assigned roles. The C++ requester then stopped at
`ACK_CLOSED` with `native state mapping differs from the source boundary`.
The canonical ONNX object had only `input_ids`, `attention_mask` and
`position_ids`; it contained no dynamic `past_key.*`/`present_key.*` tensors,
while the staged Qwen artifacts and catalog state contract require them. This
is a real preparation/protocol boundary, not a readiness result or a Python
wrapper failure. Raw requester log:
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r04/requester-0.log`
(SHA-256 `1ce55cc631af6d7543076fc3479f95ae8fab85ed896518423153502306431e52`).
The full run launcher log is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r04/launcher.log`
(SHA-256 `0b3480b3affa56837d802169f9f57eb7cca931aca33f3284f6e9d5f06df8bf1a`;
the run was cleaned by the runner after shutdown).

### Attempt r05 — native planning deadline boundary (2026-09-18)

With the dynamic-KV canonical graph, the same real MiniNDN topology again
reached both signed provider offers and passed the prior state-boundary check.
The requester then stopped at `ACK_CLOSED` with `cooperative extension deadline
exceeded`. The generated requester contract still used a fixed 5-second
`max_policy_ms`, which is not enough for source-bound planning over the 7,343
node canonical graph. Raw requester log:
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r05/requester-0.log`
(SHA-256 `4f9d60f3e3a1a506e3b75cfd649060b091d887e546892393c9593d638d43a4ed`).
This is a bounded native planning-budget boundary; it is not execution or
qualification PASS.

### Attempt r19 — coordinator handoff still unobserved (2026-09-18)

After the coordinator started reusing the authenticated V3 role projection,
the real topology again reached ACK/Selection and protected-grant verification
on both Providers. The previous empty-endpoint rejection did not recur, but no
dependency fetch/publish, assembly, ONNX execution, terminal event, or
checkpoint marker was emitted before the requester stopped with
`NATIVE_STREAM_FAILED` / `stream event gap exceeded retry budget`. Provider
logs ended after grant verification and shutdown, so the next first boundary
is still unobserved; this run does not prove the fix or product behavior.
Requester/Provider log SHA-256 values are
`cba00bff2663f65967c01cde1952e37716f4c4837d455e093f61a4e5285eeea8`,
`213754fa508971f49553bf204f929e2827835eca73e6ab6db13f57e46511c14b`, and
`716e1110d1efcbef3f64b22b998ccad91116d3cb91588402ff66e529b10dc8a9`.
This is not a product or qualification PASS.

### Attempt r20 preflight — assembly-worker digest retry boundary (2026-09-18)

The timing/DependencyObjectTrace retry stopped before MiniNDN startup at
`ASSEMBLY_WORKER_BINARY_DIGEST_MISMATCH`; no process or protocol result was
produced. The raw invocation record is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r20-preflight/launcher.log`
with SHA-256
`b617219169a4dcaaf879e3929c93833e3f4f04aaf53d1a8b8f5754ef6563ddab`.
The next invocation reads the binary digest directly instead of spelling it in
the command.

### Attempt r21 — post-grant execution boundary remains unobserved (2026-09-18)

The corrected candidate-derived assembly-worker digest and runtime/dependency
diagnostics allowed the real topology to reach signed ACK/Selection and
`NDNSF_DI_GRANT_VERIFICATION` with `boundary=BEFORE_ASSEMBLY` on both
Providers. Neither Provider then emitted a dependency fetch, assembly start,
runner-ready, execution-completed or terminal marker. The requester stopped
with `NATIVE_STREAM_FAILED` / `stream event gap exceeded retry budget`.
The logs identify the last observed boundary as protected-grant verification,
but do not identify whether coordinator entry, dependency fetch, provider
callback delivery or stream transport failed next. No ONNX execution,
hidden-state handoff, terminal response, checkpoint or cleanup baseline was
observed.

Raw log SHA-256 values are:

| Log | SHA-256 |
| --- | --- |
| `requester-0.log` | `42c94ea5f83bf28a325382c762652407b7fd01d55e9cca6ea17e8f0ce1f4ecea` |
| `provider-0.log` | `761d393b7de74cece91a145509d9faf9cf2680b6a7e4bd3bc8630e363ec9bd2c` |
| `provider-1.log` | `cff5f5af426ef0db28c22f83e0d52744d950b072bb277c4649fa097d50f1a0d6` |

This is a runtime observability/protocol boundary, not a product or
qualification PASS. A generic stream gap must not be used as the next retry's
root cause; the next candidate must emit and assert the provider-side marker
sequence required by Spec189 AD-07.

### Attempt r06 — request-scoped stream-grant boundary (2026-09-18)

After increasing the large-model policy budget to the native-supported 60
seconds, the real topology passed the candidate fence again. Controller,
Authority, both Providers and their role-specific preparation reached ready
markers; both Providers emitted signed `DI_PLACEMENT_V3_OFFER` decisions. The
C++ requester entered `Runtime.open -> User.prepare -> PreparedModel.request`,
then the stream path stopped with `NATIVE_STREAM_FAILED` because the Provider
rejected the request-scoped stream grant. No model execution, hidden-state
handoff, terminal stream event or conversation checkpoint was observed. Raw
requester log:
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r06/requester-0.log`
(SHA-256 `8bb808e73251e152741913f10955f290fbdc05dbd2359149fcbb291a1ec1fc27`).
Provider logs and the resource sample are retained beside the run; the added
diagnostic logging is for the next rebuilt candidate and does not change the
acceptance path.

### Attempt r07 — rebuilt-provider confirmation (2026-09-18)

The Core/DI targets were rebuilt after adding diagnostic rejection logs to the
Provider stream-grant verifier. The rebuilt provider entered the same real
two-provider flow, but the requester again stopped with the same
`NATIVE_STREAM_FAILED` / `request-scoped stream grant rejected` result. The
default log level did not expose the new diagnostic line, so the exact reject
subreason remains unobserved; the run is retained and the next retry enables
the runner's `NDNSF_NDN_LOG=*=ERROR` child setting. Requester log SHA-256 is
`8bb808e73251e152741913f10955f290fbdc05dbd2359149fcbb291a1ec1fc27`;
Provider-0 and Provider-1 logs are respectively
`cbc7ac22076611e870a29a1d9dc19e77d1103328dc325f95a012b2f3ac8fed77` and
`931543ac88c9d848f7441c2908df0459b636500f647ad7e25da5ae79a937316f`.

### Attempt r08 — local publication disk boundary (2026-09-18)

The diagnostic-log retry stopped earlier, during native preparation, with
`PREPARATION_FAILED / large-data file publication has insufficient reserved
disk space`. The failure was caused by stale 1.5 GB request-publication wire
files left by failed earlier runs in `/tmp/ndnsf-large-data`, not by the
candidate or Provider protocol. No stream grant verifier was reached in this
attempt. The stale files were removed after all r01–r08 processes had exited;
the raw requester log SHA-256 is
`b44420c55349cf8e7cbaccd5e28f2dab73b96d720841f010a41c3252a2de85ab`.

### Attempt r09 — stream-grant reason still unobserved (2026-09-18)

With stale publication files removed and `NDNSF_NDN_LOG=*=ERROR`, the real
two-provider request again passed preparation, both signed role offers and the
native request route, then failed with the same request-scoped stream-grant
rejection. The rebuilt Provider still did not emit the diagnostic line, so the
subreason is not yet observable; no execution or hidden-state handoff occurred.
Requester log SHA-256 is
`8bb808e73251e152741913f10955f290fbdc05dbd2359149fcbb291a1ec1fc27`;
Provider-0/Provider-1 logs are
`0fdc8ee0897a9b5e2357122848d458cf4490f8a9465261709939a4bb0611c073` and
`12bc0e0d31f7bacecf235edeeed351e150345cd3ac3043df73d45c1f83ac662d`.

### Attempt r10 — missing request-scoped grant (2026-09-18)

With direct stderr diagnostics enabled, Provider-0 reported
`NDNSF_STREAM_GRANT_REJECT reason=missing-grant` for the request-scoped stream
grant. The requester returned `NATIVE_STREAM_FAILED` / `request-scoped stream
grant rejected`; no protected model execution or terminal event occurred. This
identified a real collaboration-provider wiring defect that static review of
the request grant publication/consumption contract should have checked before
repeating the full run. Requester/Provider log SHA-256 values are
`8bb808e73251e152741913f10955f290fbdc05dbd2359149fcbb291a1ec1fc27`,
`55ab81a825b63f6c96c0412279fb12b3e656e2c7ab9f6e7220fb1b5a90449ff3` and
`add645e4d98c983068ec378978d60616af624561634af07c772c589a938833d1`.

### Attempt r11 preflight — digest spelling boundary (2026-09-18)

The retry command supplied raw hexadecimal digests, while the maintained
runner contract requires the `sha256:` prefix. It stopped immediately with
`MODEL_STAGE_MANIFEST_DIGEST_MISMATCH`. No run directory, MiniNDN process,
Controller, Authority, Provider or requester was started. The raw invocation
record is `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r11-preflight/launcher.log`.
Its SHA-256 is `ef99cc72170e02b1df1aed66b8cddb32b7749d52539ec20d1acae8de94c79d0e`.
This is a command-contract boundary and does not change the product or
qualification status; the next retry uses the exact prefixed digests.

### Attempt r12 — controller role-policy wiring boundary (2026-09-18)

After the request-scoped collaboration grant fix, the real topology reached
both role-specific preparation markers and signed placement offers. The C++
requester then stopped with `NATIVE_STREAM_FAILED` and
`Provider lacks controller-authorized collaboration role
/LLM/Pipeline/Stage/0`. The generated policy used the application root as the
role permission prefix (`/example/ndnsf-qwen06b/ROLE/...`) instead of the
service-scoped permission (`/AI/LLM/Pipeline/QwenNative/ROLE/...`). No ONNX
execution, hidden-state handoff, terminal event, or checkpoint was observed.
Requester log: `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r12/requester-0.log`.
This is an experiment policy wiring boundary; the offers do not constitute
product or qualification PASS.

### Attempt r13 — protected-grant operator closure boundary (2026-09-18)

After correcting the service-scoped role policy, both Providers passed
ACK/Selection and entered collaboration execution. Both failed while loading
the protected-grant operator credential with
`DI_PROTECTED_GRANT_REJECTED: operator file cannot be opened`; the requester
observed `stream event gap exceeded retry budget`. The runner supplied
`authority-public.pem` but did not generate the required sibling
`trust-root-registry-v1.json` and registry public-key closure consumed by
`NativeProtectedGrantCredentials`. No ONNX execution, hidden-state handoff,
terminal event, or checkpoint was observed. Log SHA-256 values are
`aabe3820403570ff39f3e7f42672237f3ebce1eeacc9d378217cc1de9cd1e0c0`,
`6ce536f8194292211326c6d7d7fb523c82bd10f94bd6444e63f56b8a015fc9d1`, and
`d97c7f3aea8fc169bfe79b72dd5dc33a67d90fa87d3ca6b22ec80b62816bd1d0` for
requester, Provider-0 and Provider-1 respectively. This is a candidate
operator-credential closure boundary, not a product or qualification PASS.

### Attempt r14 preflight — mistyped initializer digest (2026-09-18)

The command contained a mistyped canonical external-initializer digest and
stopped at `MODEL_CANONICAL_INITIALIZER_DIGEST_MISMATCH`. No run directory,
MiniNDN process, Controller, Authority, Provider or requester was started.
The raw invocation record is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r14-preflight/launcher.log`.
This is a command identity boundary, not a product or protocol result; the
next invocation reads all candidate digests directly from files.

### Attempt r16 preflight — assembly-worker digest spelling boundary (2026-09-18)

The diagnostic retry stopped before MiniNDN startup because the command
contained a mistyped `DI_NativeOnnxAssemblyWorker` digest and the runner
returned `ASSEMBLY_WORKER_BINARY_DIGEST_MISMATCH`. No run directory,
Controller, Authority, Provider or requester process was started. The raw
invocation record is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r16-preflight/launcher.log`
with SHA-256
`3aed922f546539326d7ffa089096aaafe5d9589d27df0d377310f5ea22c46490`.
This is a command identity boundary, not a product or protocol result.

### Attempt r17 preflight — privileged Python dependency boundary (2026-09-18)

The corrected command reached the runner's canonical ONNX identity check but
was launched with the root Python environment, which does not contain the
user-installed `onnx`/`numpy` modules. It stopped with
`canonical ONNX identity requires onnx and numpy`; no MiniNDN process was
started. The raw invocation record is
`.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r17-preflight/launcher.log`
with SHA-256
`ad02acf7e144f9699408cc4b07d7e7be041fe721dbc23c72eb21e5ffb76144f6`.
This is a local preflight environment boundary, not a product or protocol
result. The next invocation preserves the user Python module path while
running the network experiment as root.

### Attempt r18 — V3 endpoint propagation boundary (2026-09-18)

With the user Python site-packages preserved for the root MiniNDN launcher, the
real topology reached signed ACK/Selection and both Providers verified their
protected grants before assembly. Provider-1 then rejected its first fetch;
the diagnostic record was:
`direction=fetch role=/LLM/Pipeline/Stage/1
producer=/LLM/Pipeline/Stage/0 consumer=/LLM/Pipeline/Stage/1 endpoint=
state=1 authorized=1 expired=0 allowed=0 owns_role=1 peer_matches=0
peer_present=0`. The empty endpoint identifies a native generation-coordinator
projection bug: it reconstructed a legacy plan edge instead of reusing the
authenticated V3 endpoint. Requester status was
`NATIVE_STREAM_FAILED` / `stream event gap exceeded retry budget`; no ONNX
execution, hidden-state handoff, terminal event, or checkpoint occurred.
Requester/Provider log SHA-256 values are
`02fd269cb7e2f4797cc1804d5d3aae2d1d1bccdccc9de97c75c4e774b5d24c14`,
`90060c414841010cef7129f1dc19a2e85d4164ec72d61bc22f3b3e95dcbfcc9f`, and
`abd42fd41bb1fdc7c3dc8cb34514e3974d0efd32f4438cb05853dd51dfe3de2f`.
This is a confirmed production projection boundary, not a product or
qualification PASS.

### Attempt r15 — protected dataflow authorization boundary (2026-09-18)

After adding the authority public-key registry and rebuilding the Provider,
both Providers passed signed ACK/Selection and emitted
`NDNSF_DI_GRANT_VERIFICATION` with `boundary=BEFORE_ASSEMBLY`. Provider-1 then
failed on the first protected inter-Provider dataflow operation with
`protected dataflow is not authorized for this role/endpoint`; the requester
reported `NATIVE_STREAM_FAILED` / `stream event gap exceeded retry budget`.
Provider-0 had no successful publish event. No ONNX execution, hidden-state
handoff, terminal event, or checkpoint was observed. Raw logs are retained
under `.codex-tmp/spec189-qwen-two-provider-20260918/runs/two-provider-r15/`.
The rebuilt Provider binary SHA-256 is
`c56a680a6dbd4409bf3a17d8dd25ff69a8c87a1cca6b04c2da5989dde230d459`.
This is a protected dataflow authorization boundary, not a product or
qualification PASS.

No two-provider native execution or hidden-state handoff has been observed. A single ONNX Runtime session or a preloaded runner would not satisfy this batch.

## Five-lane coverage

| Lane | State | Current evidence / gap |
| --- | --- | --- |
| production entry/callers | `covered-partial` | requester/provider reached `Runtime.open → User.prepare → PreparedModel.request`; post-grant execution caller not observed |
| implementation/wire | `covered-partial` | V3 endpoint projection fix and policy/credential fixes are in source; fetch/assembly/terminal wire remains unobserved |
| test/harness/oracle | `gap` | no C++ full-path oracle or endpoint-preservation regression has run |
| build/source closure | `covered-partial` | affected DI targets built with the recorded receipt; Spec189 selector symbol map is incomplete |
| migration/evidence | `covered-partial` | immutable candidate and r01-r21 logs retained; repeat and cleanup evidence absent |

The C++ end-to-end oracle has not started. The four miss classes are
`static` (missing preflight/marker gates), `compile/link` (no Spec189 oracle
run), `runtime/test` (ACK/Selection/grant only) and `unobserved` (the first
post-grant boundary, fetch, assembly, execution, handoff, terminal and drain).

## Closure decision

`OPEN_FOR_NEXT_BATCH` with trigger: the C++ endpoint-preservation regression
and post-grant Provider marker sequence are implemented and reviewed, then a
fresh run identifies the first post-grant boundary or observes the complete
fetch/assembly/execute/terminal sequence. Current status is
`BLOCKED_FOR_NATIVE_EXECUTION`.
