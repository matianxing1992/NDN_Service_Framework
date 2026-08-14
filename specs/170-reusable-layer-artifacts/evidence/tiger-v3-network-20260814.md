# Spec170 Tiger V3 network evidence — 2026-08-14

## Candidate

- source/SIF base revision: `2a7b847bbeba1b628a1a9e2a146b5411f1df5556`
- OCI digest: `sha256:920ccd37d4365fcd8f5423e45112753cd8113163ebabe864b51c0695cdd20b7b`
- SIF: `/project/tma1/ndnsf-di/releases/spec170-runtime-2a7b847bbeba1b628a1a9e2a146b5411f1df5556/runtime.sif`
- SIF SHA-256: `c40cfa9abd964e9feec293093753a3de6fb32327b332bd0a873e15b50cfb6c70`
- V3 diagnostic source commits: `10520f2`, `1d5aafa`, `8f59423`, and `37f7e4c` (scripts are bind-mounted, not baked into
  the base SIF)
- provider script SHA-256:
  `3813f61bbab174755d7e3cc183dc61d4ccdb53066891d40d28c29d32ecb8207e`
- user script SHA-256:
  `ddff996aaa259613bd9ddda02f023cf111da31fffae248169b7fe87e8b1d9d3b`
- Slurm job wrapper SHA-256:
  `8383f331889c65825bedd36c41520b1a5d94033108adab466929c92d6333093b`
- semantic validator SHA-256:
  `26dba0a69111975a4a647a164dea8a011c15837b7a56a119b61356104c72a019`

The base SIF passed the static import, CUDA, Qwen, mount, and isolated-PIB/
network preflights. The base SIF is therefore retained unchanged for this
diagnostic; the V3 scripts are separately hashed release inputs.

## Local MiniNDN dependency-closure gate

The opt-in real MiniNDN gate was first run from this clean checkout with
`SPEC170_RUN_REAL_MININDN=1`. It failed before MiniNDN/NFD startup because the
checkout contains the Python package but not its compiled extension:

```text
ModuleNotFoundError: No module named 'py_repoclient._py_repoclient'
```

This is a dependency-closure failure, not a MiniNDN protocol result. As a
reversible diagnostic only, the ABI-matched Python 3.8 extension from the
existing host build was symlinked into the checkout for one run, then removed:

```text
source: /home/tianxing/NDN/ndn-service-framework/
        NDNSF-DistributedRepo/pythonWrapper/py_repoclient/
        _py_repoclient.cpython-38-x86_64-linux-gnu.so
SHA-256: b27e960b103d796345fc004aacd797a468f33ed6efc4534d1d86ef8325967cf7
```

With that extension present, the same one-request real MiniNDN topology
completed successfully (`LLM_PIPELINE_MININDN_OK`), including three Provider
ACKs, assignment selection, provider projection, selected-stage execution,
and `LLM_PIPELINE_USER_RESPONSE`. The run used the fake model runtime and
therefore proves the MiniNDN control/request path only; it does not replace
the Tiger SIF gate or prove real Qwen execution. The release candidate must
package or build this ABI-matched extension inside its own runtime closure;
relying on a host-installed extension is not acceptable for a sealed image.

The retained evidence files are under
`evidence/minindn-rootext-20260814/`:

```text
controller.log                 sha256:5a3c99cdaacfe373650c6687d6b503a825c6780eeb084a6f25f4fc1ff17e0d7d
stage0-provider.log             sha256:2e8e21c178e5cbaa3501afda9a1e3c41c65eae6845590e58416f68b872a2ba99
stage1-provider.log             sha256:eccf493864f379d1d64fb762e7de48358a62704f8a20220412260a05749eca7d
stage2-provider.log             sha256:19ef8e85c06497a6d13f266e63e464e4d8f710401befe99af4a8108f1ce03a0a
llm-pipeline-user.log           sha256:1be9f49d8ce73048b3029efe91631300df28de0fd57ab41bff0ff95bf2e790c4
llm-pipeline-user-measured.csv  sha256:32aa53168267e0744ef79651366bc3195712677d51e12cc0a975356d4e35af9e
```

## Native path diagnosis

Jobs `189427`, `189428`, `189430`, and `189431` used the old NativeTracer
driver. The real NFD/controller and all four native Providers became ready;
the user received 14 ACK candidates and the selector selected all four roles,
but no Provider Selection/Response was observed before the user deadline.

The failure is not classified as a generic container failure:

1. `native-execution-plan.json` declares `DATA_DRIVEN_V2`, while
   `native_di_tracer/user_driver.py` emits `LEGACY_READY_SET_V1` and calls the
   compatibility `request_collaboration()` path.
2. The native `DATA_DRIVEN_V2` handler requires assignment backend/device/
   artifact fields, which the legacy selector does not produce.
3. The current native V2 readiness check requires CUDA evidence, while this
   job intentionally had no GPU allocation or `--nv`; the Spec170 CPU/no-GPU
   contract is not implemented in that native path yet.
4. A V1 diagnostic variant (`189432`) failed early with the explicit error
   `LEGACY_READY_SET_V1 plan requires --require-execution-lease`, confirming
   that this is a protocol/driver contract mismatch rather than an NFD image
   startup error.

Remote native evidence is retained under:

`/project/tma1/ndnsf-di/evidence/spec170/runtime-2a7b847bbeba1b628a1a9e2a146b5411f1df5556/network-bundle/network-smoke-189427/`

and the corresponding `189428`, `189430`, `189431` directories.

## Python V3 CPU control gate

Slurm job `189434` used the same base SIF, real NFD, the same C++ controller,
four isolated Python Provider processes, and one Python User process. It used
the public V3 lifecycle:

```text
begin_collaboration(DEFERRED)
  -> ACK_CLOSED
  -> PlanSealerV3 / ProviderSelectionProjectionV3
  -> commit_plan
  -> selected Provider handlers
  -> final Response
```

Observed markers:

- four `SPEC170_V3_PROVIDER_READY` markers with `device=cpu`;
- four positive `DI_PLACEMENT_V3` ACKs;
- `SPEC170_V3_USER_PERMISSION ... allowed=5`;
- `SPEC170_V3_USER_ACK_CLOSED ... ackCount=4`;
- `SPEC170_V3_USER_SELECTION_COMMITTED`;
- selected callbacks for Backbone, Head/Shard/0, Head/Shard/1, and Merge;
- `SPEC170_V3_PROVIDER_RESPONSE` from Merge;
- `SPEC170_V3_USER_RESPONSE ... payload=V3_CPU_OK`;
- terminal marker `SPEC170_PYTHON_V3_NETWORK_PASS job=189434 user_rc=0`.

Remote evidence is retained under:

`/project/tma1/ndnsf-di/evidence/spec170/runtime-2a7b847bbeba1b628a1a9e2a146b5411f1df5556/network-bundle/python-v3-network-smoke-189434/`

This is a real-NFD V3 control/selection/response gate, not a full Gate D0:
it uses CPU diagnostic handlers and does not yet prove canonical artifact
publication, Provider-side assembly, model execution, or complete multi-token
output. Spec170 T002–T039 therefore remain open.

## CPU ONNX artifact execution gate

Job `189435` reran the same V3 network flow with `SPEC170_V3_ONNX=1`. Each
Provider loaded its mounted artifact with `onnxruntime` and
`CPUExecutionProvider` before advertising readiness, then executed the model
after Selection:

| role | artifact SHA-256 | output shape | output bytes |
|---|---|---:|---:|
| `/Backbone` | `78933d8d10878d0c1590f04e269c733c40c686595ad92fd15ba78104707ff4bc` | `[1,16]` | 64 |
| `/Head/Shard/0` | `3a8bf108bac8fddfc7edf92f5f26680e33f9c6b0e6de254eebfed43e65e6b0ef` | `[1,8]` | 32 |
| `/Head/Shard/1` | `4cd0bed590c455b44fb5903dc7b996d5216929d40c8fd242a430e8a73dc03a28` | `[1,8]` | 32 |
| `/Merge` | `874a664d460b9bba8f631e1dea8d6342d391364c27c74f5deebe65813d32fd78` | `[1,4]` | 16 |

The User again observed permission, four ACKs, V3 Selection commit, four
selected callbacks, and a final `V3_CPU_OK` Response. The job emitted
`SPEC170_PYTHON_V3_NETWORK_PASS job=189435 user_rc=0`.

The final CPU rerun (`189446`) used the corrected generic V3 scripts and
explicit ONNX Runtime thread counts. It completed with exit code `0:0` on
`itiger05`, including four `CPUExecutionProvider` runtime markers and a final
`V3_OK` response. This confirms that the later GPU-only guard changes did not
regress the CPU path. This job proves real CPU ONNX execution of pre-mounted
role artifacts, not canonical publication, cross-Provider activation dataflow,
or multi-token model generation.

## GPU ONNX artifact execution gate

The first GPU run (`189438`) used `bigTiger`, `rtx_5000:1`, node `itiger11`,
and `SPEC170_V3_DEVICE=cuda:0` with `apptainer --nv`. All four Providers
loaded and executed their mounted artifacts, and the V3 User completed
permission, four ACKs, Selection, and Merge Response. Its terminal marker was
`SPEC170_PYTHON_V3_NETWORK_PASS job=189438 user_rc=0`.

The next run (`189440`) is retained as a wrapper negative: the same runtime
flow reached a final Response, but the wrapper returned exit `13` because its
regular expression did not allow the comma-separated
`CUDAExecutionProvider,CPUExecutionProvider` list. No runtime or network
failure occurred. The wrapper was corrected without changing the candidate
image or protocol.

The corrected GPU gate (`189443`) completed with `0:0` on `itiger11` using
`TresPerNode=gres/gpu:rtx_5000:1`. The allocation recorded:

```text
GPU-36f78728-c1c0-b6c6-b0dd-51c3c9f99aac,
NVIDIA RTX 5000 Ada Generation, 560.28.03, 32760 MiB
```

Each Provider emitted:

```text
backend=onnxruntime-cuda device=cuda:0
providers=CUDAExecutionProvider,CPUExecutionProvider
cpuFallbackDisabled=true
```

The CUDA provider was first in the active provider list; the ONNX Runtime
session also set `session.disable_cpu_ep_fallback=1`. Each role executed its
artifact with the same output shapes as the CPU gate (`[1,16]`, `[1,8]`,
`[1,8]`, `[1,4]`). The User observed `allowed=5`, `ackCount=4`, V3 Selection
commit, selected callbacks for all four roles, and a final `V3_OK` Response.
The terminal marker was `SPEC170_PYTHON_V3_NETWORK_PASS job=189443 user_rc=0`.

This is a real-NFD, real-GPU, four-Provider V3 functional gate. It does not
yet establish per-operator CUDA placement through an ONNX profile, latency,
canonical artifact publication, cross-Provider activation transfer, or
multi-token generation.

## Standalone real-Qwen artifact gate

The diagnostic standalone gate uses the Spec166 three-stage Qwen ONNX set,
before attempting to place it behind NDNSF-DI. The immutable manifest digest is
`sha256:b670f8ec25df1d2521ce782f46fdf2c5aecb33bdfb1eb97e07bcb99aeca84b3d` and
the stage identities are:

| role | bytes | SHA-256 |
|---|---:|---|
| `/LLM/Pipeline/Stage/0` | 1,188,932,451 | `6f02058ba2cc420b4f11c6e7ab391451c3fbdb9bc4ca4ba8adebd633de60ec06` |
| `/LLM/Pipeline/Stage/1` | 566,602,475 | `b9e4acb15b5ea2ba009f4bd6a132ce7efb90c18b5062aadc6769bd5514f3e501` |
| `/LLM/Pipeline/Stage/2` | 1,251,892,730 | `b2cb724a2f4638fee6c008c0f657ce687a1de262b67b8cbaf351aca6033ac841` |

The standalone script and Slurm wrapper are source commit `8516c73` with
SHA-256 values `5e257e2f020dbeb54bd84a30bffd8af387885dc87d991cf9cdefdd03f015e7e6`
and `b3e37423ba3aefa089f754cc3a8483a2aa175d58837897af583b12fb37af0c3b`.

The first launch (`189452`) failed before model startup because the wrapper
used a multi-argument `test -s`; it is retained as orchestration negative
evidence. The corrected GPU launch (`189453`) reached ONNX Runtime session
creation on `itiger11` with the allocated RTX 5000, but failed closed with:

```text
This session contains graph nodes that are assigned to the default CPU EP,
but fallback to CPU EP has been explicitly disabled by the user.
```

This is a meaningful Spec170 GPU result: the real Qwen stage graph cannot yet
satisfy the `CPU fallback zero` requirement. It is not a missing-GPU or
container-visibility failure; the allocation recorded the same RTX 5000 UUID
and driver as the V3 GPU gate.

Two early CPU attempts (`189454` and `189455`) also remain negative evidence.
`189454`/`189455` show that feeding returned KV tensors, or sending a single
new token with the full-context mask, reaches the first prefill successfully
but fails on the next-token MatMul shape. This exposes a runtime contract gap,
not a model-load failure.

The corrected CPU standalone run (`189457`) uses the currently supported
`stateless-zero-full-context-recompute` path: each token recomputes all three
stages with a full input context and zero KV inputs. It completed with exit
`0:0` on `itiger11`, ONNX Runtime `1.20.0`, and generated the deterministic
two-token sequence `[81917, 304]`. Per-token times were approximately
`[161.57, 163.60]` ms in the first run and `[160.47, 163.05]` ms in the
second run. This proves the real three-stage artifacts and tokenizer can
produce a complete CPU response, but it does **not** prove KV-cache reuse,
GPU no-fallback execution, canonical publication, or NDNSF-DI network
execution.

## Qwen ONNX stage-runner correction

Before the next network artifact run, `run_qwen_onnx_stage` was tested with a
regression fixture whose ONNX output order placed `present_key.0` before the
primary stage output. The old positional `outputs[0]` selection therefore
forwarded a KV tensor as hidden state. Commit `51d928f` now binds outputs by
their semantic names (`hidden_states_out` or `logits`) and fails closed on
metadata/value-count or required-output mismatches.

Source hashes:

```text
c9030aea89ff925ec86c762b27cc486efd4e0bd66ba04cbb167d036242c72b01  examples/python/NDNSF-DistributedInference/llm_pipeline/llm_pipeline_lib.py
9664247630ab51c4293de3c2a63d45010a2f836f6bec72ab83afd3d00df873c7  tests/python/test_spec170_qwen_onnx_stage_runner.py
```

Validation: the regression test first failed with the old positional output
selection, then passed after the fix; the complete Spec170 Python lane now
passes `59 passed, 2 skipped, 1 warning`. The current Tiger SIF has not been
rebuilt from this commit, so no remote Qwen network result is attributed to
this correction yet.

## Tiger shared-stage artifact probe

To validate the corrected helper against the real artifacts before rebuilding
the SIF, the helper and probe were bind-mounted into the existing candidate
SIF `2a7b847bbeba1b628a1a9e2a146b5411f1df5556`. This is an artifact/runtime
probe, not an NDNSF network acceptance result.

CPU job `189462` ran on `itiger11` with the Spec166 three-stage Qwen ONNX set,
the pinned tokenizer, and `CPUExecutionProvider`. It completed:

```text
SPEC170_QWEN_STAGE_RUNNER_PROBE_PASS device=cpu stages=3 tokens=[81917,304]
```

The two repeated decodes returned the same tokens. The recorded per-token
times were approximately `154.84/163.37 ms` on the first decode and
`161.06/163.37 ms` on the second. The summary is retained at
`evidence/tiger-qwen-stage-runner-20260814/cpu-189462-summary.json` with
SHA-256 `cf1e2f3120b41e151809af53266de2bebba45f5627349369525b65704955c833`.

GPU job `189463` used `rtx_5000:1`, `itiger11`, and `--nv`. The allocation
reported the expected RTX 5000 UUID, but strict no-fallback ONNX session
creation failed closed because the graph still contains CPU-assigned nodes:

```text
This session contains graph nodes that are assigned to the default CPU EP,
but fallback to CPU EP has been explicitly disabled by the user.
```

This is the same model-graph compatibility block seen in standalone job
`189453`, not a missing GPU or container-visibility failure. Host-GPU evidence
and the stderr are retained under
`evidence/tiger-qwen-stage-runner-20260814/`.

## Full native build closure: installed SVS mismatch (2026-08-14)

The mandatory full examples build was configured successfully with the
repository's detected dependencies, including `libndn-svs` 0.1.0 and Boost
1.71.0, but failed before any image rebuild. `ServiceUser.cpp` and
`ServiceProvider.cpp` call the current Experimental SVS statistics API
(`getMappingFetchStats()`, `getPublicationFetchStats()`, and
`getPiggybackStats()`). The compiler resolved `pkg-config libndn-svs` to
`/usr/local/include` and `/usr/local/lib`, whose installed `SVSPubSub` header
does not expose those methods. The active `/home/tianxing/NDN/ndn-svs`
`Experimental` checkout does expose them, but it is a separate dirty checkout
and was not silently installed over the system library.

This is an external dependency/build-closure mismatch, not a Docker-versus-
MiniNDN network result. The exact commands and first errors were:

```text
./waf configure --with-examples       # PASS; libndn-svs 0.1.0, Boost 1.71.0
./waf -j"$(nproc)"                   # FAIL
ServiceUser.cpp:1300:44: error: SVSPubSub has no member named getMappingFetchStats
ServiceUser.cpp:1301:48: error: SVSPubSub has no member named getPublicationFetchStats
ServiceUser.cpp:1302:42: error: SVSPubSub has no member named getPiggybackStats
ServiceProvider.cpp:1453-1455: same three missing members
```

The release remains blocked until a temporary-prefix build using the exact
Experimental SVS source is configured and the complete NDNSF examples closure
links against that prefix. The installed system library must not be modified
in place.

The first Experimental-prefix full build then exposed two independent host
closure issues. The UAV video probe was configured with GStreamer but its
target omitted the detected `gmodule-2.0`, `ffi`, `pcre`, and `dl` libraries;
adding those explicit `use` entries allowed that target to link. The next
targets (`App_SvsLatency` and `CryptoMicrobench`) also required `dl` because
the host OpenSSL/Boost stacktrace libraries expose dynamic-loader symbols.
Those `DL` entries are source-level link-closure fixes, not runtime behavior
changes.

The same build initially used `/home/linuxbrew/.linuxbrew/bin/ld` (Binutils
2.47) with Ubuntu system GTK/GLib libraries and failed with a large set of
transitive GTK/GLib/X11/Fontconfig undefined references. A clean rebuild now
uses `/usr/bin/ld` (Ubuntu Binutils 2.34) with the same Experimental SVS
prefix. The clean full build completed successfully:

```text
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
PKG_CONFIG_PATH=/tmp/ndnsf-svs-experimental.x2d6gC/prefix/lib/pkgconfig
./waf -o /tmp/ndnsf-build-svs-experimental-systemld-20260814 configure --with-examples
./waf -o /tmp/ndnsf-build-svs-experimental-systemld-20260814 build -j4
# incremental confirmation:
'build' finished successfully (7.020s)
```

The configured source declared 32 `examples/wscript` program targets, four
DistributedRepo program targets, and the top-level GStreamer probe; both
ONNX smoke targets explicitly declared `BOOST NDN_CXX NDN_SVS ONNXRUNTIME`.
This is a local source/link-closure PASS only. It does not yet prove that the
same source is installed in a sealed OCI/SIF candidate.

### Clean locked-SVS equivalence build

To remove the remaining ambiguity about the dirty local SVS checkout, the
exact foundation lock revision was built in a separate worktree:

```text
SVS revision: 060811333de68b9674e45522222a14d4e047bf28
SVS library SHA-256: 2d3dfa0edcecb2d06bcd5c3123f88827bcef750613475bb036d6546578ec66f0
linker: /usr/bin/ld (GNU Binutils for Ubuntu) 2.34
Boost: 1.71.0
NDNSF build: ./waf ... configure --with-examples; ./waf ... build -j4
result: 'build' finished successfully (14m29.589s), 306/306 tasks
```

The clean locked SVS build included the two ONNX smoke executables and the
full examples/DistributedRepo closure. This confirms that the current NDNSF
source links against the exact SVS revision consumed by the foundation build;
the earlier missing-statistics failure was caused by selecting the stale
installed `/usr/local` SVS, not by MiniNDN networking. The temporary worktree
and prefix are retained for this evidence record only; the active dirty SVS
checkout was not modified.

The source-level Spec170 Python regression suite also passed at this candidate:

```text
PYTHONPATH=NDNSF-DistributedInference:pythonWrapper \
  python3 -m pytest -q tests/python/test_spec170_*.py
59 passed, 2 skipped, 1 warning in 5.65s
```

## Sealed foundation and GPU image handoff

The GitHub Actions foundation workflow completed successfully as run
`31782722011` from source revision
`07debb71ed5fbe5d208ae27291f063906e68a07a`. Its immutable manifest records:

```text
image: ghcr.io/matianxing1992/ndnsf-di-spec170-foundation@sha256:b94c323f0e0fbd10f0a1fc48faffaba7aa3e277ae2100567937b4dd65a61c96f
manifestDigest: sha256:2bcf5ea32dbd5b60ab6cd66407ea07ecf32a4e75351b1597377c1f2ce8c7f40e
lockDigest: sha256:f98df291346ef1bec2554ea924a4fc6b8f2d75cb012281b4960ded033804a050
base: ubuntu@sha256:8feb4d8ca5354def3d8fce243717141ce31e2c428701f6682bd2fafe15388214
cosign: verified with the GitHub Actions OIDC identity
```

The GPU image workflow was then dispatched as run `31784722395` using that
foundation digest, the locked foundation source revision above, and the
current `Experimental` source head
`db1601ab8614677107ba65a001cb1a029363e555`. It is queued; no Tiger SIF or
real-Qwen claim is made until this workflow produces and verifies its own
immutable runtime manifest.

## Next gate

The next implementation gate is to resolve the real Qwen artifact/runtime
contract: either make every required graph node CUDA-capable for the D1
no-fallback requirement, or explicitly keep the run CPU-only and document the
GPU gate as `BLOCK`; then wire the full-context stage dataflow into the V3
Provider projections. T030/T031 and the later reuse/cross-Provider/hybrid
gates remain open; the current evidence is diagnostic and has not created a
Spec170 frozen candidate.
