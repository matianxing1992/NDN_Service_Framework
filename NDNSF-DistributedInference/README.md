# NDNSF-DistributedInference

NDNSF-DistributedInference is an application-layer distributed inference
runtime built on NDNSF. The current user-facing API and examples are still
Python-first, but the performance direction is C++-first: hot-path scheduling,
dependency dataflow, prefetch, worker dispatch, and future ONNX Runtime
execution should run in native C++ and call NDNSF Core directly. Python should
remain a thin API, deployment, GUI, and experiment layer.

The repository therefore contains two layers today:

```text
Stable APP-facing layer
  Python APPClient / APPProvider / APPController / APPDeployment
  policy generation, MiniNDN experiments, GUI, and example scripts

Native hot-path migration layer
  C++ async dataflow runtime under NDNSF-DistributedInference/cpp/
  multi-worker role scheduling, planned dependency edges, fan-in/fan-out
  execution, and timing hooks for future C++ ONNX/NDNSF integration
```

The long-term goal is not to wrap a Python executor around NDNSF forever.
Instead, Python should describe the service and submit inference jobs, while
the providers execute dependency-driven inference through native C++ workers.

Deployment status, operator commands and evidence boundaries are maintained in
[`docs/NDNSF-DI-deployment-candidate.md`](../docs/NDNSF-DI-deployment-candidate.md).
Spec 105 currently closes as `minindnCandidateOverall=BLOCK`; physical
production stays `DEFERRED` to Spec 106. Simulated Runtime v1 behavior is only
available under `ndnsf-di contract-smoke`; production adapters must consume the
bound profile/plan/request/output command-template fields and fail closed.

Layering:

```text
APP
  calls model/inference APIs

NDNSF-DistributedInference
  understands model plans, roles, stages, shards, runtime artifacts,
  backend requirements, and inference dependencies; current public API is
  Python, while the hot-path runtime is migrating to C++

NDNSF-DistributedRepo
  stores model/runtime/intermediate objects with controlled replication,
  artifact references, storage capabilities, and placement policy

NDNSF Python Wrapper
  exposes generic service invocation, collaboration, encrypted artifacts,
  segmented large data, and provider callbacks for Python clients/tools

NDNSF Core
  handles Face, SVS, NAC-ABE, signing, permissions, selection, workers,
  and wire protocol behavior
```

## First-Time End-to-End Guide

This section is for a developer who has not used NDNSF-DI before. The short
version is:

```text
1. Choose or generate a service policy.
2. Review identities, users, providers, roles, and artifacts.
3. Start one controller process.
4. Start one or more provider processes.
5. Start a user/client process and call distributed_inference(...).
6. Check the result and provider logs.
```

The application developer should stay at the APP layer. They should not need
to hand-write NDN Interest names, Data names, SVS topics, segment names,
NAC-ABE attributes, or permission Interest names. Those are derived from the
service policy and handled by NDNSF-DI plus NDNSF Core.

### 1. Install the Python API

From the repository root:

```bash
python3 -m pip install -e ./pythonWrapper
python3 -m pip install -e ./NDNSF-DistributedInference
```

For ONNX examples, install the model/runtime dependencies used by the example
environment, such as `numpy`, `onnx`, `onnxruntime`, and the model package that
exports the ONNX graph. The MiniNDN scripts assume the repository build and
NDNSF native runtime are already available in the same checkout.

### 2. Understand the Main Objects

The public APP API has four main entry points:

```text
APPDeployment   reads the policy and generates deployment files
APPController   runs the NDNSF service controller for this deployment
APPProvider     advertises provider capability and executes assigned roles
APPClient       submits inference requests and receives final outputs
```

A service policy, such as `yolo_policy.yaml`, is the central contract. It says:

```text
which service name is being provided
which users may invoke that service
which providers may provide that service
which model roles/stages/shards exist
which role depends on which previous role
which model/runtime artifacts each role needs
how request and response payloads are encoded
```

For a user, the service call stays simple:

```python
result = client.distributed_inference("/AI/YOLO/SplitInference", image_tensor)
```

The policy decides the rest: provider selection, role assignment, artifact
fetching, activation exchange, and final response collection.

`NxM` layout terminology is reserved for the real distributed-inference target:
`N` vertical model stages and `M` parallel shards inside each stage. Shards in
the same stage should run on different providers concurrently, then exchange or
merge the tensors required by the next stage/frontier. A model-specific splitter
must therefore emit horizontal/tensor-sharded ONNX chunks and a fan-in/fan-out
dependency graph. A sequential chain of chunks is not considered true NxM
parallel sharding. Splitters can use `nxm_stage_roles(...)` and
`nxm_stage_frontier_dependencies(...)` as the generic stage-frontier skeleton,
then fill in model-specific tensor names, merge semantics, and artifacts.

### 2.1 Native Hot-Path Runtime Direction

The Python executor remains useful for experiments, policy validation, and
model-specific splitter prototyping, but it is not the right long-term hot path.
The native migration is staged:

```text
Step 1: C++ async dataflow runtime
  Role frontier scheduling, fan-in/fan-out readiness, multi-worker execution,
  planned dependency edge metadata, and timing records.

Step 2: C++ NDNSF integration
  Use ServiceUser/ServiceProvider, large-data references, pending Interests,
  and deterministic activation names directly from C++ instead of crossing the
  Python wrapper for every role callback.

Step 3: C++ backend runners
  ONNX Runtime CPU execution is now available as a minimal native role runner.
  Python can still create policies and submit jobs, but provider execution
  should move toward native runners.
```

`NDNSF-DistributedInference/cpp/ndnsf-di/AsyncDataflowRuntime.hpp` is the first
native graph-level building block. It is intentionally model-agnostic: a role
runner can be an ONNX chunk, a PyTorch-exported native runner, a containerized
function, or a future accelerator backend. The runtime only enforces dependency
readiness and parallel execution semantics.

`NDNSF-DistributedInference/cpp/ndnsf-di/ProviderRoleWorker.hpp` is the
provider-side hot-path boundary. When a provider receives an assigned role, the
worker starts prefetch for all planned input edges immediately, waits until all
required inputs are available, runs the native role runner, and publishes every
declared output edge. Source roles can also receive request-level initial input
bundles directly through the native session/runtime API, so the first stage no
longer has to fake a dependency edge just to get the user's input tensor. Its
`DependencyIo` interface is the place where C++ NDNSF large-data fetch/publish
and pending-Interest support attaches. This keeps Python out of the per-edge
execution loop while preserving the existing Python-facing API.

`NDNSF-DistributedInference/cpp/ndnsf-di/NativeModelRunner.hpp` defines the
backend boundary. Provider scheduling and dependency I/O do not need to change
when switching from a test runner to an ONNX chunk runner. It also defines
`NativeModelRunnerSpec` and `RegistryNativeModelRunnerFactory`, so
deployment/artifact metadata can be converted into a role runner through a
narrow backend registry.

`NDNSF-DistributedInference/cpp/ndnsf-di/TensorBundleCodec.hpp` defines the
native multi-tensor activation format. It stores tensor name, dtype, shape, and
payload for each tensor, so C++ providers can exchange a YOLO-style bundle with
multiple backbone/head tensors instead of assuming every activation is a single
raw byte vector. Provider workers can also use tensor names carried by
dependency edges to publish an edge-specific subset from a larger ONNX output
bundle.

`NDNSF-DistributedInference/cpp/ndnsf-di/OnnxRuntimeModelRunner.hpp` and
`OnnxRuntimeModelRunner.cpp` provide the first real native backend. The current
adapter supports CPU ONNX Runtime execution for float32 raw tensor bundles and
the native multi-tensor bundle codec. It reads ONNX input/output names and
optional shape/scope metadata from `NativeModelRunnerSpec`, runs the ONNX chunk
in C++, and returns output `TensorBundle`s for the dependency executor. This is
enough to prove that the native hot path can execute an actual ONNX model
without crossing the Python wrapper for the role computation. It is still
intentionally narrow: native tensor-bundle input/output is currently limited to
float32 tensors and richer shape/type negotiation is future work. The Python
provider path can materialize repo-backed artifacts into a local provider cache.
The C++ native provider executable now uses the same role-scoped artifact
reference file in serving mode: it reads `repoManifest` / `largeDataReference`
metadata, fetches repo-backed segmented Data through the manifest's Data names
and forwarding hints, verifies size/hash, caches the materialized artifact, and
runs from the cached path. In serving mode it registers the collaboration
service before materialization completes and reports `runtimeStatus=installing`,
`ready`, or `failed` through readiness ACK payloads; normal selection only sees a
positive ACK after the native runner specs have been installed and the C++
handler has been attached. `--check-only` remains an offline/local-path check.

Artifact provisioning is a generic provider/session lifecycle, not a
llama-server-specific behavior. Any large model or runtime bundle may be
installed asynchronously: ONNX shards, GGUF files, safetensors directories,
TensorRT engines, runtime executables, or future container bundles. During
installation the provider can publish negative readiness ACKs with
`runtimeStatus=installing`; after the artifacts are fetched, verified, cached,
and any managed runtime is started, the same readiness probe returns ready and
normal service selection may choose the provider. This keeps expensive
hundreds-of-MB/GB provisioning out of the hot inference request path.

`NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderRuntime.hpp` is the
provider process facade, with implementation in `NativeProviderRuntime.cpp`.
It owns the worker pool and the role-to-runner registry. Deployment/Python code
should eventually register native runners for the roles a provider can execute,
then submit assigned `RoleSpec` objects to this runtime. That is the intended
"C++ core, thin Python API" shape.

`NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderSession.hpp` is the
native provider skeleton boundary, with implementation in
`NativeProviderSession.cpp`. It combines a generated execution plan, provider
assignment, `DependencyIo`, runner factory, and provider runtime. A future
network-serving provider should load the generated plan, register role runners
from artifact metadata, then execute assigned roles through this session
instead of wiring those pieces by hand. It already supports source-role initial
inputs, dependency-driven intermediate roles, and final role result publication.

Every header under `NDNSF-DistributedInference/cpp/ndnsf-di/` now has a matching
`.cpp` translation unit. Stateful runtime classes such as
`ProviderRoleWorker`, `NativeProviderRuntime`, and `NativeProviderSession` keep
their implementation out of the header. Smaller codec/plan helper headers still
use inline helpers where appropriate, but their corresponding `.cpp` files are
compiled by the native DI targets so the C++ project has a real source layout.

`NDNSF-DistributedInference/cpp/ndnsf-di/NativeProviderHandler.hpp` adapts that
session shape to `ServiceProvider::CollaborationContext`. It constructs the
per-request `NdnsfCollaborationDependencyIo`, executes the assigned native
role, publishes inter-role activation outputs through planned dependency edges,
and keeps the final role's user-visible result on the normal NDNSF response
path with `publishFinalResponse(...)`.

`examples/DI_NativeProviderSessionSmoke.cpp` is a small native executable smoke
for this boundary. It uses a fake backend and in-memory dependency I/O to run a
`/Backbone -> /Head/* -> /Merge` plan without MiniNDN or NFD:

```bash
./waf build --targets=di-native-provider-session-smoke
build/examples/di-native-provider-session-smoke
```

`examples/DI_NativeOnnxRuntimeSmoke.cpp` is the corresponding real-backend
smoke. It loads a small ONNX model, runs it through
`OnnxRuntimeModelRunner`, and verifies the float32 output entirely in C++:

```bash
./waf configure --with-examples --with-tests
./waf build --targets=di-native-onnxruntime-smoke
build/examples/di-native-onnxruntime-smoke /tmp/ndnsf-di-add-one.onnx
```

The expected success line is:

```text
NDNSF_DI_NATIVE_ONNXRUNTIME_SMOKE_OK 2,3,4
```

`examples/DI_NativePlanManifestSmoke.cpp` checks the generated-plan handoff. It
loads `native-execution-plan.json` and `service-manifest.json`, creates fake
role runners from the manifest, runs the `/Backbone -> /Head/* -> /Merge`
frontier through in-memory dependency I/O, and verifies that tensor-level edge
metadata can drive C++ bundle selection:

```bash
./waf build --targets=di-native-plan-manifest-smoke
build/examples/di-native-plan-manifest-smoke \
  /tmp/ndnsf-di-yolo-policy/native-execution-plan.json \
  /tmp/ndnsf-di-yolo-policy/service-manifest.json \
  /AI/YOLO/2x2Inference
```

`examples/DI_NativeProviderExecutable.cpp` is the first native provider
executable. Its `--check-only` mode loads
`native-execution-plan.json`, loads `service-manifest.json`, materializes local
ONNX artifact paths by constructing `OnnxRuntimeModelRunner` instances, and
registers every role in a `NativeProviderSession`:

```bash
./waf build --targets=di-native-provider
build/examples/di-native-provider \
  --plan /tmp/ndnsf-di-yolo-policy/native-execution-plan.json \
  --manifest /tmp/ndnsf-di-yolo-policy/service-manifest.json \
  --artifact-references /tmp/ndnsf-di-yolo-policy/artifact-references.json \
  --artifact-cache-dir /tmp/ndnsf-di-native-artifacts \
  --service /AI/YOLO/2x2Inference \
  --provider /NDNSF-DistributeInference/example/provider/A \
  --workers 4 \
  --check-only
```

`--artifact-references` is optional. In `--check-only` mode it verifies and
caches local payload paths. In `--serve` mode it also handles repo-only
references: the executable waits for permission to `/NDNSF/DistributedRepo`,
reads each role's embedded `repoManifest`, fetches the signed segmented Data
named by the manifest with NDN `SegmentFetcher`, applies forwarding hints when
needed, verifies size/hash, writes the artifact under `--artifact-cache-dir`,
and rewrites the runner spec to the materialized path. A shared-service JSON
`FETCH` fallback remains for legacy manifests without segment locations, but
large model/runtime artifacts should use the manifest-aware segmented Data path.

Its `--serve` mode registers the same native session as an NDNSF collaboration
provider. It still uses normal NDNSF permissions, tokens, ACK/Selection/Response,
planned large-data activation names, and `ServiceProvider::CollaborationContext`;
only the provider execution hot path is native C++:

```bash
build/examples/di-native-provider \
  --serve \
  --plan /tmp/ndnsf-di-yolo-policy/native-execution-plan.json \
  --manifest /tmp/ndnsf-di-yolo-policy/service-manifest.json \
  --service /AI/YOLO/2x2Inference \
  --provider /NDNSF-DistributeInference/example/provider/A \
  --group /NDNSF-DistributeInference/example/group \
  --controller /NDNSF-DistributeInference/example/controller \
  --roles /Head/Shard/0 \
  --workers 4
```

The top-level `wscript` checks for an optional `onnxruntime` pkg-config package.
When the C++ development package is present, the unit-test target links the
native adapter. When it is not present, registering the `onnxruntime` backend
still gives a clear runtime error instead of silently falling back to Python.

`NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlan.hpp` mirrors the
deployment plan in C++. It converts role/dependency metadata plus a
session/provider assignment into role-local `RoleSpec` objects with
deterministic planned activation object names, deterministic segment Data names
when the segment count is static, expected segment counts, and expected byte
counts. This is the handoff point from Python policy/deployment code into the
native provider runtime.  The intended API boundary is:

```text
deployPlan(nativeExecutionPlan, serviceManifest, providerAssignment)
  -> planSessionId + role-local deterministic edge plan

invoke(planSessionId, inputReference, inferenceId)
  -> source role receives only the new input reference; providers reuse the
     already installed runner specs, role mapping, tensor edge list, object
     name templates, and static segment counts.
```

In other words, model artifacts, runner metadata, role assignments, edge tensor
lists, object-name templates, and static segment counts are deployment/session
state.  They should not be republished for every inference.  Per-inference
messages should carry only the changing input reference, inference/session id,
and security context needed for that run.

`NDNSF-DistributedInference/cpp/ndnsf-di/NativeExecutionPlanJson.hpp` loads the
generated `native-execution-plan.json` file into those C++ plan objects. The
JSON loader is intentionally narrow and only reads the native hot-path fields,
including tensor names and `segmentNaming` on dependency edges, so C++
providers do not need to parse the full deployment YAML.

`NDNSF-DistributedInference/cpp/ndnsf-di/NativeServiceManifest.hpp` loads the
generated `service-manifest.json` artifact metadata into `NativeModelRunnerSpec`
objects. It preserves each role's backend, kind, artifact path/reference, and
scalar metadata such as `input_tensors` and `output_tensors`. This lets the C++
runtime consume planner/deployment output directly instead of hand-writing
runner specs in tests. When the generated manifest points at repo-backed
artifacts, `NativeArtifactMaterializer` uses the native provider's repo fetch
adapter to materialize them before runner construction.

`NDNSF-DistributedInference/cpp/ndnsf-di/NdnsfCollaborationDependencyIo.hpp`
is the first Core-facing adapter. It maps `DependencyIo` to
`ServiceProvider::CollaborationContext`: planned input names are fetched with
`fetchLarge(...)`, and planned output names are published with
`publishLargeNamed(...)`. If an edge has a static `expectedSegments` value, the
Core fetch path expands the planned activation object name into exact segment
Interests (`segment=0..N-1`) immediately; if an edge is dynamic, the runtime
starts from the planned object name and follows the object's final block. This
exact-segment mode is the default whenever `expectedSegments > 0`; set
`NDNSF_COLLAB_LARGE_EXACT_SEGMENT_FETCH=0` only for comparison experiments.
Exact segment fetch uses `NDNSF_COLLAB_LARGE_EXACT_SEGMENT_WINDOW` (default
`64`) and `NDNSF_COLLAB_LARGE_EXACT_SEGMENT_INTEREST_LIFETIME_MS` (default
`5000`) so large static activation bundles can retry missing segments without
waiting for the full collaboration fetch timeout.
This is still a building block rather than a complete C++ ONNX provider, but it
fixes the intended ownership boundary: DI execution logic can be native C++,
while NDNSF Core remains responsible for segmented large data, pending
Interests, encryption, permissions, and wire behavior.

Dependency transfer boundary: DI model artifacts, runtime bundles, initial
inputs, and activation tensor bundles are exact-name objects. The normal path
is NDNSF large-data reference, repo materialization, `publishLargeNamed(...)`,
`fetchLarge(...)`, and SegmentFetcher-style retrieval. Tensor bundles are
finite planned objects, so the runtime publishes their bytes directly on this
path. It does not wrap them in the continuous-publication stream protocol;
that protocol is not a replacement for exact-name object retrieval.

These native components do not change NDNSF Request/ACK/Selection/Response
semantics and do not add AI-specific behavior to NDNSF Core.

Current distance to the target architecture:

```text
Done:
  C++ async frontier runtime
  C++ provider role worker pool
  C++ native execution-plan loader
  C++ provider session skeleton
  C++ NDNSF CollaborationContext adapter
  C++ ONNX Runtime CPU runner for float32 raw tensors
  C++ multi-tensor bundle codec for native activation exchange
  tensor-level dependency metadata in native-execution-plan.json
  C++ service-manifest loader to NativeModelRunnerSpec
  C++ generated plan + service manifest smoke
  native provider executable check-only path for local artifacts
  native provider executable NDNSF serving path for local artifacts
  native provider executable repo-backed artifact fetch/materialization
  MiniNDN YOLO 2x2 parallel-detect smoke with native compute providers
  request initial-input injection for source roles
  service-level Python API wrapper around the native executable
```

Therefore the project is past the "headers only" stage: the native C++ runtime
can execute real ONNX computation locally, validate generated plans, and replace
YOLO 2x2 parallel-detect compute providers in a MiniNDN smoke while fetching
model artifacts from NDNSF-DistributedRepo. It is not yet fully C++-first
because controller/user orchestration and the user-facing deployment API remain
Python-facing.

### 3. Create or Inspect a Policy

A policy can be written by hand, generated by a model-specific splitter, or
generated by an ONNX-assisted planner. The current YOLO examples ship
ready-to-run policy files and splitter scripts. The current YOLO layout
splitter exports a real Ultralytics YOLO model into sequential ONNX chunks and
uses the dependency executor after deployment. It is useful for validating
NDNSF-DI role assignment, artifact provisioning, large-data activation exchange,
and deterministic dependency names, but it is not yet a true NxM tensor-parallel
splitter. For true NxM, the splitter must generate multiple parallel shards per
stage and explicit merge/fan-in edges. For network regression, the generated
policy creates enough compute provider identities for the generated roles while
keeping the repo provider separate.

The `yolo_2x2` splitter now exposes two experimental parallel modes. The older
`--parallel-output-shards` mode is a small correctness scaffold: same-stage
roles run independently and a `/Merge` role concatenates output slices, but
Stage-0 shards duplicate the upstream YOLO backbone. The newer
`--parallel-detect-scale-shards` mode is closer to a real YOLO partition: one
shared `/Backbone` chunk computes the backbone/neck once, parallel
`/Head/Shard/*` chunks run YOLO Detect scale branches, and `/Merge` decodes the
final predictions. It is still model-specific, but it exercises the intended
fan-out/fan-in dependency executor without pretending that every ONNX model has
a rectangular shard layout. Merge fan-in edges use producer-local key scopes
such as `detect-head-shard0-to-merge` and `detect-head-shard1-to-merge`, so the
native C++ hot path can prefetch and store each planned input independently
instead of collapsing multiple producers into one scope.

For larger inputs, the splitter also supports
`--parallel-detect-replicated-backbone-shards`. This mode removes the shared
`/Backbone` role: each `/Head/Shard/*` chunk runs the needed backbone/neck work
locally and only publishes compact detection-candidate tensors to `/Merge`.
It intentionally duplicates compute to avoid large Backbone-to-Head activation
transfer. On the current `yolo26n.pt` 640x640 experiment, this is the better
NDN tradeoff: the plan publishes only two Head-to-Merge activation objects,
about 202 KB and 30 planned segments total, instead of the shared-backbone
plan's roughly 3.07 MB and 441 planned segments.

The preferred planner entry point for these two Detect modes is now
`--auto-parallel-detect-plan`. It generates both the shared-backbone and
replicated-backbone candidates, estimates each candidate's critical compute
time, activation bytes, planned segment count, provider RTT, and transfer time,
then writes `planner-selection.json` and uses the lower estimated-latency plan.
The printed `YOLO_LAYOUT_PLANNER_CANDIDATE` lines are the audit trail; the
chosen candidate is also embedded in the generated service metadata.

For a two-stage YOLO split:

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_split/split_model.py
```

For the four-role YOLO 2x2 example:

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/split_model.py
```

For a custom YOLO ONNX layout:

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/split_model.py \
  --layout 3x2 \
  --out-dir /tmp/ndnsf-yolo-3x2 \
  --policy /tmp/ndnsf-yolo-3x2/yolo_policy.yaml
```

The splitter exports one ONNX chunk per generated role, writes chunk-level
dependencies from ONNX chunk input/output tensors, and runs a local chunk
pipeline verification before reporting success.

Before deployment, review the policy:

```bash
PYTHONPATH="NDNSF-DistributedInference:$PYTHONPATH" \
python3 -m ndnsf_distributed_inference.policy \
  --config examples/python/NDNSF-DistributedInference/yolo_2x2/yolo_policy.yaml \
  --out-dir /tmp/ndnsf-di-review \
  --print-summary
```

The summary shows:

```text
User permissions      which services each user may invoke
Provider permissions  which services and roles each provider may run
Role coverage         whether every role has at least one provider
Artifact coverage     which roles have model/runtime artifacts
Artifact security     whether executable artifacts are allowed
```

This is a deployment sanity check. Real authorization still comes from each
service's exact `users` and `providers` lists.

### 4. Start the Controller

The controller reads the same policy and publishes the NDNSF permission and
trust material for the deployment:

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_split/controller.py \
  --config examples/python/NDNSF-DistributedInference/yolo_split/yolo_policy.yaml
```

In a MiniNDN script, this process runs on one node. In a real deployment, it
runs on the node that owns the service-controller identity and trust root.

### 5. Start Providers

Providers register the service and advertise which roles they can run:

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_split/provider.py \
  --config examples/python/NDNSF-DistributedInference/yolo_split/yolo_policy.yaml \
  --provider-id A \
  --temp-dir /tmp/ndnsf-yolo-provider-A
```

For homogeneous provider pools, `roles="all"` is normally enough. A provider
does not need to start with every model shard installed locally. If the policy
contains artifacts or artifact references, the provider can be selected for a
role and then download the role-specific model/runtime artifact.

### 6. Start the User

The user process uses `APPClient` and calls the service name:

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_split/user.py \
  --config examples/python/NDNSF-DistributedInference/yolo_split/yolo_policy.yaml
```

Inside the application, the user-facing code looks like:

```python
client = APPClient.from_config("yolo_policy.yaml")
service = "/AI/YOLO/SplitInference"
print(client.describe_input(service))
print(client.describe_output(service))
result = client.distributed_inference(service, image_tensor)
```

If the service input declares `codec: npz`, NDNSF-DI can encode common numpy
tensor inputs automatically. Register a custom input encoder only when the
model needs custom preprocessing.

### 7. Run the Full MiniNDN Smoke Tests

The easiest way to exercise the whole network path is MiniNDN:

```bash
sudo -E python3 Experiments/NDNSF_DI_YoloSplit_Minindn.py
sudo -E python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py
sudo -E python3 Experiments/NDNSF_DI_PyTorch2x2_Minindn.py
```

The YOLO layout smoke can replace the Python compute providers with the native
C++ provider executable for the parallel-detect layout:

```bash
sudo -E python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py \
  --layout 2x2 \
  --parallel-detect-scale-shards \
  --native-providers \
  --cold-requests 1 \
  --warm-requests 1
```

Expected success markers include:

```text
YOLO_SPLIT_RESULT ... ok=true
YOLO_2X2_RESULT ... ok=true
PYTORCH_2X2_RESULT ... ok=true
```

The unified runner can also launch selected cases. The `yolo-layout` case is
the MiniNDN network-level custom-layout regression; `yolo-layout-local` is the
fast non-MiniNDN layout/policy smoke:

```bash
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case runtime-compat
sudo -E python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case all
sudo -E python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case yolo-layout --layout 2x3
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case yolo-layout-local --layout 3x2
```

Current validated YOLO layout coverage:

```text
2x2  default historical network regression
2x3  MiniNDN network regression with one provider per generated role
3x2  MiniNDN network regression with one provider per generated role
1x3  local export/policy/ONNX correctness smoke
3x3  local export/policy/ONNX correctness smoke; run yolo-layout before using it as a release baseline
2x2 parallel-detect-scale  local ONNX correctness smoke with /Backbone, /Head/*, /Merge
2x3 parallel-detect-scale  local policy/ONNX smoke; MiniNDN smoke is the network baseline
```

Layouts in the historical YOLO regression are written as `ROWSxCOLS`, but their
metadata is marked `layout_semantics: pipeline-sequential-chunks` and
`stage_shards_parallel: false`. Do not use those YOLO numbers as evidence for
true NxM parallel sharding. The planner still emits a YOLO-specific sequential
chunk plan; it is not yet a fully generic "arbitrary ONNX graph to arbitrary
parallel distributed layout" planner.

To generate the experimental parallel-output prototype:

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/split_model.py \
  --layout 2x2 \
  --parallel-output-shards \
  --out-dir /tmp/ndnsf-yolo-parallel-2x2
```

To generate the YOLO Detect-scale DAG splitter:

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/split_model.py \
  --layout 2x3 \
  --parallel-detect-scale-shards \
  --out-dir /tmp/ndnsf-yolo-detect-scale-2x3
```

For a larger YOLO input where cross-node activation dominates, generate the
replicated-backbone Detect plan:

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/split_model.py \
  --layout 2x2 \
  --model yolo26n.pt \
  --input-size 640 \
  --parallel-detect-replicated-backbone-shards \
  --out-dir /tmp/ndnsf-yolo-detect-replicated-2x2
```

To let the planner choose between shared-backbone and replicated-backbone:

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/split_model.py \
  --layout 2x2 \
  --model yolo26n.pt \
  --input-size 640 \
  --auto-parallel-detect-plan \
  --out-dir /tmp/ndnsf-yolo-detect-auto-2x2
```

### 8. Common Deployment Mistakes

If deployment fails, check these first:

```text
runtime.user_identity is not listed in any service users list
provider identity is not listed in the service providers list
service roles mention a role that no provider can run
artifact path points to a missing file
repo manifest refers to a model/runtime object that was not published
trust.anchor_file is missing in a production deployment
input tensor shape or dtype does not match describe_input(service)
provider starts with can_provision=False but no local model shard exists
```

Use:

```bash
python3 -m ndnsf_distributed_inference.policy \
  --config yolo_policy.yaml \
  --out-dir /tmp/ndnsf-di-review \
  --explain
```

to catch most policy-level mistakes before starting controller, providers, or
users.

## Graphical Deployment Tool

For users who are not comfortable editing YAML by hand, NDNSF-DI also includes
a lightweight Python GUI:

```bash
PYTHONPATH="NDNSF-DistributedInference:$PYTHONPATH" \
python3 Experiments/NDNSF_DI_GUI.py
```

For a GUI-oriented preflight launcher that can also run a MiniNDN regression
before opening the GUI, use:

```bash
python3 Experiments/NDNSF_DI_GUI_Minindn.py
python3 Experiments/NDNSF_DI_GUI_Minindn.py --run-minindn --case app-api --no-gui
python3 Experiments/NDNSF_DI_GUI_Minindn.py --run-minindn --case yolo-2x2 --no-gui
```

The first command checks `tkinter`, imports the GUI, validates the default
policy, and opens the GUI. The second command runs a quick non-MiniNDN API
smoke through the same launcher. The third command runs the full YOLO 2x2
MiniNDN distributed-inference diagnostic path.

The first version uses the Python standard-library `tkinter` toolkit so it can
run on a normal Ubuntu desktop without adding a Qt dependency. It provides:

```text
Project Wizard
  Import an ONNX/PyTorch/policy file, choose service/controller/group/user
  names, choose providers and roles, and generate a policy skeleton.

Policy Editor
  Load and edit YAML, browse users/providers/services, validate the policy,
  and show the same --explain summary as ndnsf-di-policy.

Model Split
  Import an ONNX model, display graph summary and candidate split points, and
  seed a two-stage policy skeleton.

Certificate / Identity Manager
  Run ndnsec list, select a local identity for runtime.user_identity, generate
  key requests, and import safebags. Certificate signing is still expected to
  follow the deployment trust process; the GUI only wraps common ndnsec tasks.

Controller / User / Provider certificate tools
  The role tabs include the deployment certificate workflow. A User or
  Provider tab can generate its own private key and key request, then copy the
  request text to the Controller tab. If the current node is the root/controller
  node, the Controller tab can generate the root certificate and sign pasted or
  file-based requests. The signed certificate can then be copied back to the
  User or Provider tab and installed with ndnsec cert-install. This keeps the
  private key on the requesting node while allowing the root/controller node to
  sign deployment identities.

Deployment Runner
  Launch example controller/provider/user processes, show logs, and run the
  unified DI regression runner. The default YOLO 2x2 regression starts MiniNDN,
  runs distributed inference, and checks for YOLO_2X2_RESULT ... ok=true.
  The auto-split two-stage regression is also available as a selectable case.
```

The same GUI also has role-specific `Controller`, `User`, and `Provider` tabs.
A real node can enable any combination of these roles: for example, one desktop
can run the controller and user, while another worker runs one or more provider
roles. These tabs configure and launch the APP-level role processes from the
same policy file, then send their logs to the Deployment Runner pane.

The GUI is intentionally built on the APP-level APIs and the existing
`ndnsf-di-policy` validation path. It does not introduce a separate policy
format or a second authorization mechanism.

## Application-Level API

The recommended API for application developers is `APPClient`,
`APPProvider`, `APPController`, and `APPDeployment`.
These names hide
NDN-specific concepts such as Face, SVS, trust schema files, permission
Interests, and artifact Data names. An APP developer describes:

```text
service name
model parts / roles
runtime backend
local handler logic for each provider role
```

The distributed-inference layer maps that description onto NDNSF service
invocation, artifact publishing, role assignment, encrypted shared scopes, and
provider callbacks.

Each service name is unique and corresponds to exactly one model layout: one
model identity, one role set, and one dependency graph. If the same model is
split in a different way, it should be published as a different service name.
The dependency graph therefore lives in the deployment config, not in each
request.

In the example below, `APPClient` is the application-side client facade. It
does not ask application code to construct a Face, SVS group, or permission
fetcher by hand; those lower-level NDNSF runtime objects are derived from the
configuration file.

`yolo_policy.yaml` is the deployment policy. It can be generated by a model
splitter or by an application deployment tool. A model splitter is an offline
deployment-time tool: it reads the original model or model description, decides
which stages/shards the model should be split into, which artifact each role
needs, and what dependency graph connects the intermediate results, then emits
policy YAML that NDNSF-DI can load. The YOLO example includes such a splitter
script. For a new model family, the application or framework developer can
write their own splitter as long as it emits the standard service, roles,
dependencies, and artifacts description.

`APPClient.from_config("yolo_policy.yaml")` reads that YAML, generates the
corresponding trust schema, controller policy, and service manifest, and uses
those generated files to connect the lower-level `DistributedInferenceClient`.
Service packages should ship a default policy config for their service, as the
YOLO examples do with `yolo_policy.yaml`. Users only need to pass a custom
config when they change identities, trust roots, provider pools, artifact
paths, artifact references, or the model split itself.

Client side:

```python
from ndnsf_distributed_inference import APPClient

client = APPClient.from_config("yolo_policy.yaml")
service = "/AI/YOLO/SplitInference"
print(client.describe_input(service))
print(client.describe_output(service))

# If the policy says codec=npz and declares one tensor field, NDNSF-DI can
# encode a numpy tensor automatically. Register a custom encoder only when the
# service needs model-specific preprocessing.
result = client.distributed_inference(service, image_tensor)

# Multiple requests can be submitted concurrently. Each request still uses the
# NDNSF runtime for Face/SVS/NAC-ABE work; the APP thread receives a Future.
future = client.async_distributed_inference(service, image_tensor)
result = future.result(timeout=30)
```

The recommended client entry point is `distributed_inference(...)`; the
asynchronous form is `async_distributed_inference(...)`. These names are
intentional: this APP layer exposes distributed inference, not a generic NDN
service invocation API. The caller passes the service name and an application
object such as `image_tensor`, and NDNSF-DI converts the object to request bytes
according to the service input description. The service name is a constrained
application contract, not an arbitrary string:
`/AI/YOLO/SplitInference` is bound in `yolo_policy.yaml` to one model identity,
model version, input/output encoding, role set, dependency graph, provider
identities, and security policy. In other words, the client does not repeat
the model split, stages, or dependencies on each request; those are looked up
from the service name.

For the user, this is intentionally the whole request API. Provider selection,
role assignment, artifact publication, model-shard download, scope-key
distribution, dependency exchange, and result collection are runtime work
hidden behind the service call. The default deployment assumption is that
providers start as homogeneous workers for a service. If the service policy
contains artifact descriptions, `distributed_inference(...)` automatically builds a
dynamic provisioning plan: selected providers fetch the executable/runtime
bundle and the model shard for the role they are assigned. If the service
policy has no artifacts, the same call falls back to the pre-deployed-model
path.

`client.describe_input(service)` returns the input description recorded in the
policy, such as the codec, field names, shape, dtype, and recommended encoder
name. `client.describe_output(service)` returns the response description, such
as the output codec and tensor layout. For common tensor payloads, a built-in
NPZ encoder is available: if the policy declares `codec: npz`, callers may pass
a numpy tensor, a mapping from field names to tensors, or a tuple/list matching
the declared fields. Use `register_input_encoder(...)` only for model-specific
preprocessing that the generic NPZ encoder cannot know. If callers already have
encoded bytes, they can pass those bytes to the same
`distributed_inference(...)` or `async_distributed_inference(...)` entry point.

The encoded request bytes are defined by the service contract. They are created
by built-in or application-level encoding logic. NDNSF-DI does not interpret
the bytes after encoding; it securely carries them through the distributed
execution flow for the selected service. The provider handler and client must
use the same payload schema for a service. If the input shape, dtype,
preprocessing, or model split changes incompatibly, publish a new service name
or model version instead of reusing the old name.

Model/runtime artifacts belong to the service definition, not to ad-hoc user
code. The splitter or deployment tool should write artifact paths or repo
manifests into the service policy. Application callers normally do not build a
`DistributedInferencePlan` manually; they call `distributed_inference(...)` and
let the APP layer derive the plan from the service policy. Advanced deployment
tools can still call `client.service_plan(service, ...)` when they need to
inspect or reuse the generated plan. The optional `artifact_references`
argument to `distributed_inference(...)`, `async_distributed_inference(...)`,
and `service_plan(...)` refers to model/runtime artifacts stored in
NDNSF-DistributedRepo, not to input images or activation tensors. The
`artifact_references` name reflects that the same entry carries both
`repoManifest` and `largeDataReference` metadata. Inputs and intermediate
tensors use the service payload contract plus NDNSF large-data or
dependency-object helpers.
Repo-backed artifacts still fetch through the manifest-aware repo path, but the
execution spec also carries the same large-data reference metadata shape used
by inputs and activations. New planner or executor code should consume that
reference metadata instead of passing naked Data-name strings.
The APP plan builder now carries the reference in each artifact spec first; the
embedded repo manifest remains as fetch metadata for repo-backed providers and
as a compatibility fallback for older scripts.
The metadata has a `source` field: `repo-manifest` means the provider should
use the repo manifest-aware object fetch path, while `ndn-large-data` means the
provider can fetch the named encrypted large Data directly.
Generated repo deployment manifest files write both fields explicitly for each
artifact: `repoManifest` for the manifest-aware fetch path and
`largeDataReference` for human/planner inspection of source, Data name, hash,
and size. The runtime execution spec also carries these camelCase fields while
keeping legacy snake_case aliases for older providers. New provider code should
prefer `largeDataReference` and only fall back to `repoManifest` or
`repo_manifest` for compatibility.

Provider side:

```python
from ndnsf_distributed_inference import APPProvider

provider = APPProvider.from_config("yolo_policy.yaml", provider_id="A")
provider.serve_service(
    service="/AI/YOLO/SplitInference",
    roles="all",
    handler=handle_assigned_role,
    backends=["onnxruntime"],
    temp_dir="/tmp/ndnsf-yolo-provider-A",
    has_model=False,
    can_provision=True,
)
provider.run()
```

Providers use one service-level registration. In the common homogeneous-worker
case, every provider can advertise `roles="all"` with `can_provision=True`.
The selected provider for a role downloads the role artifact from the
assignment and becomes capable of executing that role for the request. A
deployment that has already installed model shards locally can instead pass
`has_model=True, can_provision=False`.

Provider-side Python handlers can also use a separate worker pool:

```python
provider = APPProvider.from_config(
    "yolo_policy.yaml",
    provider_id="A",
    handler_workers=4,
)
```

The NDNSF callback waits for the worker result so the collaboration context
remains valid, while expensive Python model logic runs outside the NDNSF
callback function itself.

Controller side:

```python
from ndnsf_distributed_inference import APPController

controller = APPController.from_config("yolo_policy.yaml")
controller.run()
```

Deployment-only utilities:

```python
from ndnsf_distributed_inference import APPDeployment

deployment = APPDeployment.from_config("yolo_policy.yaml")
print(deployment.trust_schema)
print(deployment.policy_file)
```

For multi-service deployments, call `provider.serve_service(...)` once per
service and use `client.async_distributed_inference(...)` for concurrent
requests across one or more services. The service name still determines the
fixed role set and dependency graph used for each request.

## Example Families

The repository currently includes three Python example families under
`examples/python/NDNSF-DistributedInference/`:

```text
yolo_split/
  Two-stage real Ultralytics YOLO split inference over ONNX Runtime.

yolo_2x2/
  Layout-driven real Ultralytics YOLO ONNX inference with a separate repo node.
  The historical default is 2x2, but the splitter accepts custom layouts such
  as 1x3, 2x3, 3x2, and 3x3. It exports one real ONNX chunk per generated role,
  builds dependencies from actual chunk input/output tensors, and verifies the
  local chunk pipeline against the full YOLO reference before writing policy.
  A controller-side deployer stores model/runtime artifacts in the repo before
  inference. Providers fetch their assigned artifacts on demand and execute the
  same dependency-driven role handler regardless of layout.

pytorch_eager_2x2/
  Four-provider fully connected ONNX example generated from a PyTorch-defined
  model. The splitter exports a full ONNX reference graph, analyzes candidate
  cut points, then writes four ONNX shards: two hidden-layer shards and two
  output-layer shards. It verifies the distributed result against a local
  full-model reference.
```

The ONNX examples represent the preferred portable deployment path when a
model can be exported to ONNX. The fully connected example shows why some
model families still need model-specific splitters: a generic sequential ONNX
cut can find graph boundaries, but horizontal splitting inside a dense layer
requires knowledge of weight rows, activation offsets, and output merge order.

The lower-level `DistributedInferenceClient`,
`DistributedInferenceProvider`, and `DistributedInferenceController` remain
available for framework developers and experiments that need direct control.

## DistributedRepo Integration

Model shards, runtime bundles, and intermediate data should not be pushed to
arbitrary nodes. NDNSF-DI can carry artifact references generated from
NDNSF-DistributedRepo object manifests:

```python
from ndnsf_distributed_inference import (
    LocalDistributedRepo,
    PlacementPolicy,
    StorageCapability,
)

repo = LocalDistributedRepo([
    StorageCapability("/repo/A", free_bytes=4_000_000_000,
                      recent_load=0.1, failure_domain="rack-a"),
    StorageCapability("/repo/B", free_bytes=4_000_000_000,
                      recent_load=0.2, failure_domain="rack-b"),
])

manifest = repo.put(
    object_name=(
        "/NDNSF-DistributeInference/example/controller/NDNSF-DISTRIBUTED-REPO/OBJECT/"
        "NDNSF-DI/ARTIFACT/AI/YOLO/2x2/Stage/0/Shard/0/model"
    ),
    payload=model_bytes,
    object_type="onnx-model",
    policy=PlacementPolicy(replication_factor=2),
    policy_epoch="/Policy/yolo-2x2/v1",
)

payload = repo.fetch_object(manifest.object_name, manifest)
```

The manifest records the object hash, size, replication factor, selected repo
nodes, and the signed Data names that hold the object segments. Object names
remain publisher-scoped: controller-published artifacts are named under the
controller object namespace, user-published inputs/intermediates are named
under the user namespace, and provider-published outputs are named under the
provider namespace. The Data names that actually serve stored segments may be
repo-owned, e.g. `/repo-node/NDNSF-DISTRIBUTED-REPO/DATA/<object-hash>`, so
fetchers can route directly to the selected repo node. In the current
networked path, a controller-side deployer stores model/runtime artifacts in a
repo node before inference. The user request carries only execution specs and
artifact references. Each reference includes a `largeDataReference` plus a
`repoManifest` for manifest-aware fetch; selected providers fetch their own
assigned artifacts from the repo and cache them locally.
DI code should prefer the manifest-aware object API (`fetch_object()` /
`get_object()`) when reading model artifacts, runtime bundles, images, or
activation objects. That API returns one verified logical object and hides
whether the repo stored it as one Data packet, many segmented Data packets, or
a replicated object. The planner and dependency graph should therefore talk in
terms of object references and manifests, not repo segment names.

## Lower-Level API Sketch

User side:

```python
from ndnsf_distributed_inference import (
    DistributedInferenceClient,
    load_or_generate_deployment,
)
from ndnsf import CollaborationRole

deployment = load_or_generate_deployment("yolo_policy.yaml", "/tmp/yolo-policy")
service = deployment.service_policy("/AI/YOLO/SplitInference")
client = DistributedInferenceClient.connect(
    group=deployment.group,
    controller=deployment.controller,
    user=deployment.user,
    trust_schema=deployment.trust_schema,
)
request_payload = encode_image_for_yolo(image_tensor)
graph = deployment.dependency_graph_for_service(service.name)
result = client.infer_deployed_service(
    service.name,
    request_payload,
    roles=[CollaborationRole(role=role, service=service.name)
           for role in service.roles],
    key_scopes=graph.key_scopes(),
    dependencies=list(service.dependencies),
    role_scopes=graph.role_scopes(),
)
```

For a generated `DistributedInferencePlan`, prefer the explicit plan-session
API when running repeated inference against the same deployed layout:

```python
session = client.deploy_plan(plan)

for image in images:
    result = client.invoke_plan(
        session,
        encode_image_for_yolo(image),
        ack_timeout_ms=300,
        timeout_ms=10000,
    )
```

`deploy_plan(...)` installs or reuses static model/runtime artifacts, key-scope
material, role metadata, dependency tensor lists, object-name templates, and
static segment-count hints. `invoke_plan(...)` sends only the changing input
payload/reference plus the normal NDNSF collaboration request.

Provider side:

```python
from ndnsf_distributed_inference import (
    DistributedInferenceProvider,
    load_or_generate_deployment,
)

deployment = load_or_generate_deployment("yolo_policy.yaml", "/tmp/yolo-policy")
inference = DistributedInferenceProvider.create(
    provider_id=deployment.provider_id_for_role("/Stage/0"),
    group=deployment.group,
    controller=deployment.controller,
    provider_prefix=deployment.provider_prefix,
    trust_schema=deployment.trust_schema,
)
inference.add_role("/AI/YOLO/SplitInference", "/Stage/0", handle_stage0)
inference.run()
```

Controller side:

```python
from ndnsf_distributed_inference import (
    DistributedInferenceController,
    load_or_generate_deployment,
)

deployment = load_or_generate_deployment("yolo_policy.yaml", "/tmp/yolo-policy")
controller = DistributedInferenceController.create(
    controller_prefix=deployment.controller,
    policy_file=deployment.policy_file,
    trust_schema=deployment.trust_schema,
    bootstrap_identities=deployment.bootstrap_identities,
)
controller.run()
```

The APP, model publisher, or model-splitting tool owns the semantic service
definition: how the model is split, which roles exist, what each role publishes
or waits for, and what runtime/backend is required. NDNSF-DistributedInference
does not require every model to use the same dependency-generation mechanism.
It accepts dependency graphs emitted by handwritten splitters, PyTorch-specific
splitters, ONNX analyzers, container-bundle planners, or future optimizers. The
runtime carries the dependency graph recorded in the service config and
converts the plan into generic NDNSF collaboration calls and artifact
provisioning.

For users who are not familiar with NDN, the intended API boundary is:

```text
Application code:
  APPClient / APPProvider / APPController
  SplitterOutput or yolo_policy.yaml
  execute_onnx_dependency_chunk(...) for ONNX role handlers

Framework/internal code:
  NDNSF request/ACK/selection/response names
  segmented large-data fetch/publish
  repo segment names and placement details
  NAC-ABE attributes and permission Interests
```

In other words, an AI application developer should describe the model layout,
roles, artifacts, dependencies, and input/output codecs. They should not need
to manually build NDN names or fetch individual Data segments in normal use.
If a handler has to call `ctx.ndnsf.wait_one(...)` or `ctx.ndnsf.fetch_large_reference(...)`
directly, that is a sign that the APP/runtime helper is still too low-level for
that workload.

## Dependency Graph Generation Roadmap

There are three different graphs in a distributed inference deployment:

```text
Model dependency graph
  Operator/tensor DAG inside the original model, such as an ONNX graph.

Chunk collaboration graph
  Provider-role graph after the model is partitioned. Each edge records which
  activation tensors cross from one chunk to another.

Deployment plan
  Mapping from roles to providers, runtime artifacts, artifact references,
  security policy, and NDNSF service names.
```

The current policy format keeps the chunk collaboration graph in
`services[].dependencies`. For non-ONNX models, this can still be supplied by a
model-specific splitter or application planner. For ONNX models, NDNSF-DI now
provides an optional `onnx_graph` helper module:

```text
ONNX graph
  -> tensor/operator dependency DAG
  -> candidate split points and boundary tensor costs
  -> exported ONNX chunks
  -> chunk-level dependencies with tensor names
  -> NDNSF-DI collaboration plan
```

This helper is deliberately optional. It does not replace `SplitterOutput`, and
it does not make NDNSF-DI an ONNX-only framework. Instead, it gives ONNX
deployments a common starting point for automatic planning while preserving the
same policy format for PyTorch eager, model-specific, and containerized
workloads.

The YOLO 2x2 splitter now writes an ONNX graph summary JSON beside its exported
chunks. The file has three top-level sections:

```text
fullModel
  The original exported ONNX graph: inputs, outputs, initializers, nodes,
  tensor producers, tensor consumers, and static tensor shape/size metadata.

splitCandidates
  Candidate sequential cuts ranked by unknown boundary tensors, known boundary
  bytes, number of boundary tensors, and cut position. These candidates are
  planning hints; the current YOLO splitter still chooses and exports its
  model-specific chunks.

plannerRecommendations
  Candidate/provider assignments ranked with provider compute score, estimated
  RTT/bandwidth, activation size, and compute-balance cost. This turns graph
  analysis into a planning input without changing `SplitterOutput`.

chunkGraph
  The actually exported chunks and the tensor names that cross each selected
  chunk boundary.
```

For the current YOLO 2x2 example, the default planner intentionally assumes
homogeneous providers. This keeps the focus on distributed inference mechanics:
real graph analysis, activation boundaries, artifact provisioning, and
multi-role execution. Runtime provider profiling is a later extension.

For experiments, the splitter can also accept an optional coarse provider
profile JSON:

```json
{
  "providers": [
    {
      "name": "/NDNSF-DistributeInference/example/provider/A",
      "compute_score": 1.0,
      "uplink_mbps": 200,
      "downlink_mbps": 200,
      "rtt_ms": 20
    },
    {
      "name": "/NDNSF-DistributeInference/example/provider/B",
      "compute_score": 1.0,
      "uplink_mbps": 200,
      "downlink_mbps": 200,
      "rtt_ms": 20
    }
  ]
}
```

This is intentionally an estimate, not a hard performance guarantee. Later
profiling can replace these coarse values with measured provider throughput,
model-layer latency, memory pressure, and link quality.

### Remaining Work Toward Real Distributed Computing

The current NDNSF-DI prototype already performs real network-level distributed
inference: a model can be split into ONNX stages/chunks, providers exchange
named activation objects, and MiniNDN regressions validate end-to-end results.
The next work should not be more unrelated demos. It should focus on three
framework-level steps:

1. Generate a more faithful dependency graph from the ONNX tensor DAG. The
   analyzer should preserve branch, skip-connection, concat, multi-input, and
   multi-output tensor dependencies so the chunk collaboration graph reflects
   the real model graph rather than a hand-written pipeline approximation.
   The current `build_chunk_dependencies(...)` helper already builds chunk
   edges by matching every exported chunk's ONNX outputs against every other
   chunk's ONNX inputs, so fan-out/fan-in dependencies are represented directly
   when boundary tensor names are preserved.

2. Use the planner to generate 2-stage and 2x2 policies automatically. The
   hand-tuned YOLO policies should become examples or fallbacks; the main path
   should be:

   ```text
   ONNX tensor DAG -> candidate split points -> chunk graph -> NDNSF-DI policy
   ```

3. Run comparative experiments. The important comparisons are single-node
   inference, 2-stage split inference, 2x2 split inference, different activation
   sizes, different RTT/bandwidth settings, and different provider counts.
   The comparison harness starts with a local full-ONNX baseline and can
   optionally invoke MiniNDN split regressions:

   ```bash
   python3 Experiments/NDNSF_DI_Compare_Yolo_Plans.py \
     --iterations 5 \
     --output results/yolo_di_comparison/result.json

   sudo -E python3 Experiments/NDNSF_DI_Compare_Yolo_Plans.py \
     --include-minindn-auto-split \
     --output results/yolo_di_comparison/result-with-minindn.json
   ```

Provider scheduling is intentionally not the immediate research bottleneck.
The planner exposes `ProviderProfile` and `homogeneous_provider_profiles(...)`
as a compatibility interface. By default, providers are treated as homogeneous,
which keeps current experiments focused on distributed inference mechanics.
Future runtime profiling can replace these defaults with measured compute,
memory, latency, bandwidth, and RTT values without changing the policy format.

In generated policies, a dependency may include a `tensors` field:

```yaml
dependencies:
  - producers: [/Stage/0/Shard/1]
    consumers: [/Stage/1/Shard/0]
    key_scope: stage0-to-stage1
    topic_prefix: /activation
    tensors: [x, saved_4]
```

This means the role-level edge carries a large activation object that contains
the listed tensors. The request itself should carry only small references.
Images, activations, model shards, and runtime bundles must not be embedded as
large inline NDNSF payloads. Once they exceed the inline/single-segment
threshold, DI uses the NDNSF Core large-data abstraction: a
`LargeDataReference` points to hybrid AES-GCM encrypted, signed segmented NDN
Data, or to an NDNSF-DistributedRepo manifest that stores the same kind of
opaque segmented Data. Compression or precision reduction may be used by an
application as a model-quality tradeoff, but it is not the transport mechanism
for large objects.

## Splitter Output Contract

Real dependency graphs should come from the model splitter. A splitter is
normally model-family or backend specific: a YOLO ONNX splitter, a transformer
pipeline splitter, a tensor-parallel LLM splitter, or a container-bundle
splitter may all use different logic. NDNSF-DistributedInference only requires
that the splitter emits a standard `SplitterOutput`:

```python
from ndnsf_distributed_inference import InferenceDependency
from ndnsf_distributed_inference.splitter import (
    SplitArtifact,
    SplitServiceSpec,
    SplitterOutput,
)

split = SplitterOutput(
    application="yolo-split-demo",
    controller="/NDNSF-DistributeInference/example/controller",
    group="/NDNSF-DistributeInference/example/group",
    user="/NDNSF-DistributeInference/example/user",
    provider_prefix="/NDNSF-DistributeInference/example/provider",
    trust_app_roots=["/NDNSF-DistributeInference/example"],
    services=[
        SplitServiceSpec(
            name="/AI/YOLO/SplitInference",
            model_name="/Model/Ultralytics/YOLO/Split",
            roles=["/Stage/0", "/Stage/1"],
            dependencies=[
                InferenceDependency(
                    producers=["/Stage/0"],
                    consumers=["/Stage/1"],
                    key_scope="stage0-to-stage1",
                    topic_prefix="/activation",
                ),
            ],
            artifacts=[
                SplitArtifact(
                    role="/Stage/0",
                    path="yolo-stage0.onnx",
                    artifact_name="/Model/Ultralytics/YOLO/Stage/0",
                    kind="onnx-model",
                    backend="onnxruntime",
                ),
            ],
        ),
    ],
)
split.write_policy_config("yolo_policy.yaml")
```

The generated YAML is therefore deployment policy derived from the split. The
split may be generated from an ONNX tensor DAG, from a PyTorch/model-specific
splitter, or from a handwritten application planner. The same service name
should always map to the same model layout and dependency graph. If a model is
split differently, publish it as a different service name. Splitter output uses
specific user and provider identities, matching NDNSF controller policy
semantics: each named identity receives explicit service permissions.

Provider handlers receive a role-local dependency view as `ctx.dependencies`,
so handler code can ask what the current role should publish or wait for
instead of repeating topic strings by hand:

```python
def handle_assigned_role(ctx):
    if ctx.dependencies.outputs:
        activation = run_local_stage(ctx.execution.path("model"), ctx.request)
        ctx.publish_output_large_reference(
            activation,
            data_topic_suffix="activation",
            ref_topic_suffix="ref",
            object_type="application/x-ndnsf-di-activation+npz")

    if ctx.dependencies.inputs:
        future = ctx.prefetch_input_large(topic_suffix="ref")
        activation = ctx.wait_prefetched_input_large(future)
```

For roles with multiple inputs or outputs, pass `key_scope` explicitly, e.g.
`ctx.dependencies.input("stage0-to-stage1")` or
`ctx.publish_output(payload, key_scope="stage1-internal")`.

## User-Facing Security Config

Application developers do not need to hand-write NDN validator trust schema.
They describe the deployment in YAML or JSON:

```yaml
# Deployment-specific section: edit these names for your deployment.
# application: local label for this APP deployment.
application: yolo-split-demo
# controller: NDNSF ServiceController identity/prefix that signs and
# distributes permissions.
controller: /NDNSF-DistributeInference/example/controller
# group: NDN-SVS group prefix shared by controller, users, and providers.
group: /NDNSF-DistributeInference/example/group

runtime:
  # user_identity: identity used when this config starts a user/client process.
  # This does not grant permission by itself; service users below do that.
  user_identity: /NDNSF-DistributeInference/example/user
  # provider_prefix: naming helper used by examples to derive concrete
  # providers. It is not a wildcard permission.
  provider_prefix: /NDNSF-DistributeInference/example/provider

trust:
  # app_roots: trust-schema roots for this deployment namespace.
  app_roots: [/NDNSF-DistributeInference/example]
  # Production deployments should use an explicit trust anchor.
  # anchor_file: /path/to/root.cert

artifact_security:
  # Executable artifacts are rejected unless all three are configured:
  # trust.anchor_file, artifact_security.allowlist, and sandbox.
  allowlist: []
  sandbox:
    kind: ""

authorization_summary:
  users:
    - identity: /NDNSF-DistributeInference/example/user
      services:
        - /AI/YOLO/SplitInference
  providers:
    - identity: /NDNSF-DistributeInference/example/provider
      services:
        - service: /AI/YOLO/SplitInference
          roles: all

services:
  - name: /AI/YOLO/SplitInference
    model: /Model/Ultralytics/YOLO/Split
    # users: exact identities allowed to invoke this service.
    users: [/NDNSF-DistributeInference/example/user]
    # providers: exact identities allowed to provide this service. roles=all
    # means this provider may be assigned any role listed below.
    providers:
      - identity: /NDNSF-DistributeInference/example/provider
        roles: all
      - identity: /NDNSF-DistributeInference/example/provider/A
        roles: all
    # Generated by splitter/planner. Regenerate instead of hand-editing when
    # the model split changes.
    roles: [/Stage/0, /Stage/1]
    dependencies:
      - producers: [/Stage/0]
        consumers: [/Stage/1]
        key_scope: stage0-to-stage1
        topic_prefix: /activation
```

The distributed-inference layer compiles this config into an NDNSF controller
policy and an NDN trust schema. `runtime.user_identity` selects the local
identity used by this client process; it does not authorize that identity by
itself. Service authorization uses exact per-service `users` and `providers`
entries, consistent with NDNSF policy: a named identity receives specific
permissions for specific service names and roles. Generated data and
certificate rules use hierarchical validation: Data must be named under the
signer identity, and a child certificate must be named under its parent
certificate namespace. In production, `trust.anchor_file` must point at the
trust-root certificate; the local example fallback exists only for ephemeral
self-signed demo identities.

Policy files generated by a splitter are organized into two visible sections:
`# editable deployment section` contains names, controller/group prefixes,
runtime identity, trust, and artifact-security settings; `# generated
model-plan section` contains service users/providers plus model roles,
dependencies, artifacts, input, and output. Deployment operators should edit
exact `users` and `providers` when assigning permissions, but should regenerate
roles/dependencies/artifacts when the model split changes. `ndnsf-di-policy`
also validates that `runtime.user_identity` appears in at least one service
`users` list, so a policy that looks syntactically correct but grants the local
client no service permission fails before deployment files are emitted. The
same validation checks that every declared or dependency-referenced service role
has at least one authorized provider, so a plan cannot silently require a role
that no provider is allowed to run. The optional `authorization_summary` is a
read-only review aid generated from `services[].users/providers`; it lets a
deployer quickly see which services each user can invoke and which services and
roles each provider can run. It is not a second permission source.

```bash
ndnsf-di-policy \
  --config examples/python/NDNSF-DistributedInference/yolo_split/yolo_policy.yaml \
  --out-dir /tmp/ndnsf-di-yolo-policy
```

For a pre-deployment review without opening the YAML, print the derived
authorization and coverage summary:

```bash
ndnsf-di-policy \
  --config examples/python/NDNSF-DistributedInference/yolo_split/yolo_policy.yaml \
  --out-dir /tmp/ndnsf-di-yolo-policy \
  --print-summary
```

`--explain` is an alias for `--print-summary`. The report lists user-to-service
permissions, provider-to-service/role permissions, role coverage, artifact
coverage, and artifact-security status. The command uses the same parser and
validation path as deployment generation, so missing user authorization or a
role without an authorized provider is reported before the summary is printed.

Generated files:

```text
/tmp/ndnsf-di-yolo-policy/trust-schema.conf
/tmp/ndnsf-di-yolo-policy/controller.policies
/tmp/ndnsf-di-yolo-policy/service-manifest.json
/tmp/ndnsf-di-yolo-policy/service-manifest.json.sha256
/tmp/ndnsf-di-yolo-policy/native-execution-plan.json
/tmp/ndnsf-di-yolo-policy/native-execution-plan.json.sha256
```

The service manifest is a canonical JSON form of the service-to-model and
service-to-dependency mapping. The `.sha256` file is only a local fingerprint
for deployment tooling; it is not a security signature. Security comes from
publishing the manifest as NDN Data and validating the Data signature.

`native-execution-plan.json` is narrower than the service manifest. It is the
handoff artifact for the C++ hot path and uses execution-plan schema v2. The
plan records `modelFamily` and `plannerKind` before the role graph so the
runtime can distinguish generic ONNX DAG plans, YOLO-specific plans, and future
LLM plans without changing the role/dependency schema again. The current YOLO
planner emits `modelFamily: yolo-onnx` and planner kinds such as
`yolo-detect-auto`, `yolo-detect-shared-backbone`, or
`yolo-detect-replicated-backbone`.

The native plan still contains only fields needed to construct native
`RoleSpec` objects: service name, roles, dependency producers/consumers, key
scopes, topic prefixes, deterministic object-name templates, expected segment
counts, expected byte counts, and planner identity metadata. For LLM pipeline
plans, schema v2 also carries explicit `executionMode`, `llmPipeline`, and
`roleMetadata` fields so each role can declare its stage index, stage count,
optional transformer layer range, input kind, and output kind without changing
the generic dependency representation. It is generated from the policy;
deployment operators should edit the policy or splitter inputs, not this file.
LLM pipeline execution is still a stub: the schema is ready for sequential
stage execution, but real transformer-stage runtimes are future work.

Planner dispatch is centralized through `PlannerBackendRegistry`. A planner
backend is keyed by `plannerKind` and declares its `modelFamily`; model-specific
packages keep their own splitter code behind that backend. The current YOLO
example registers sequential chunks, output-channel shards, shared-backbone
Detect shards, replicated-backbone Detect shards, and auto Detect planning in
one YOLO registry. A future LLM planner should add a new backend for the LLM
model family instead of adding YOLO-specific conditionals to the deployment
path.

Planner backends use a common `PlannerRequest` / `PlannerResult` contract.
`PlannerRequest` carries model family, model format, planner kind, model path,
output directory, layout, input size, provider profiles, and backend options.
`PlannerResult` carries the model-specific split plan plus normalized score
summary and selected-candidate metadata. The YOLO splitter now enters through
this contract even though the underlying YOLO splitting algorithms are still
model-specific. This keeps the deployment path stable while allowing future
LLM planners to return a different split plan shape.

Model family and model format are separate. YOLO currently uses
`modelFamily: yolo-onnx` with `modelFormat: onnx`; an LLM plan may use
`modelFamily: llm` with `modelFormat: hf-transformers`, `gguf`,
`safetensors`, `onnx`, or a deployment-specific custom format. The format is
planner/runtime metadata and does not force every model family to use ONNX.

The repository includes a stub LLM planner for dispatch testing. It generates
abstract pipeline, prefill/decode, or tensor-parallel roles and dependencies,
but it does not execute LLM inference, manage KV cache, or stream tokens. The
pipeline mode models ordered stage execution, where each provider owns one
stage and passes a hidden-state reference to the next stage:

```bash
PYTHONPATH=NDNSF-DistributedInference \
python3 examples/python/NDNSF-DistributedInference/llm_stub/plan_stub.py \
  --planner-kind llm-pipeline \
  --model /Model/Llama/Stub \
  --model-format gguf \
  --runtime-backend llama.cpp \
  --stages 3 \
  --layers 24 \
  --out-dir /tmp/ndnsf-di-llm-stub
```

To verify that the generated schema can drive ordered provider-stage
execution, run the local pipeline smoke:

```bash
python3 Experiments/NDNSF_DI_LlmPipeline_Smoke.py \
  --stages 3 \
  --layers 24
```

This smoke still uses a fake in-memory LLM runtime. It validates
`Stage0 -> Stage1 -> ... -> StageN` dependency execution from the generated
`native-execution-plan.json`, including `roleMetadata` layer ranges and
hidden-state dependency names, without claiming real transformer inference.

For a local preflight check of true layer-by-layer execution, the optional
Transformers smoke can compare a full decoder-only forward pass with staged
execution over contiguous layer ranges:

```bash
NDNSF_DI_TRANSFORMERS_MODEL=/path/to/local/qwen-or-llama-hf-model \
python3 Experiments/NDNSF_DI_TransformersPipeline_LocalSmoke.py \
  --stages 2
```

This smoke currently targets Llama/Qwen-style HuggingFace
`AutoModelForCausalLM` models with `model.layers`. If `transformers` or a local
model path is unavailable, it prints a skipped marker. Passing this smoke is
the local runtime prerequisite before wiring a real layer-pipeline LLM provider
into MiniNDN.

For a dependency-light regression, the same script can construct a tiny random
`LlamaForCausalLM` locally and compare full-model logits with two pipeline
stages:

```bash
python3 Experiments/NDNSF_DI_TransformersPipeline_LocalSmoke.py \
  --self-test-tiny-llama \
  --stages 2
```

This case is included in the quick suite as
`di-transformers-pipeline-local`. It validates real transformer block
execution semantics without downloading a large Qwen/Llama checkpoint.

For LLM deployments, "model plus inference engine" is the real execution unit,
but compatibility is not as uniform as ONNX plus ONNX Runtime. NDNSF-DI records
the model format explicitly so planners and runtimes can reject invalid
combinations early. Common combinations are:

| Model format | Typical engines | Notes |
| --- | --- | --- |
| `safetensors` / HuggingFace layout | Transformers, vLLM | Broadest server-side compatibility; vLLM is common for OpenAI-compatible serving. |
| `gguf` | llama.cpp, Ollama | Common local deployment format; not a direct vLLM input. |
| TensorRT-LLM engine | TensorRT-LLM runtime | High performance, but requires model conversion/build steps. |
| MLX format | MLX-LM | Apple local inference ecosystem. |

These are planner/runtime compatibility facts, not NDNSF wire-protocol facts.
The DI planner may choose different split strategies for the same model family
depending on whether the artifact is `safetensors`, `gguf`, TensorRT engine, or
another configured format.

Compatibility validation is not the same as model splitting.  Two models may
both be valid `safetensors + vLLM` deployments while still needing different
planning logic because their block layout, attention variants, KV-cache shape,
MoE routing, tensor-parallel constraints, or supported engine features differ.
The intended design is therefore layered: the common contract rejects impossible
artifact/runtime pairs, then a model-family or model-specific planner chooses
pipeline, prefill/decode, tensor-parallel, expert-parallel, or another strategy.
Future Llama/Qwen/DeepSeek planners can share the same contract but remain
separate registry backends when their structure requires it.

The stub planner now validates the requested artifact/runtime pairing before it
generates a deployment plan. For example, `--model-format safetensors
--runtime-backend vllm`, `--model-format gguf --runtime-backend llama.cpp`, and
`--model-format tensorrt-engine --runtime-backend tensorrt-llm` are accepted,
while `--model-format gguf --runtime-backend vllm` fails with a compatibility
error. This is validation only; the stub still does not launch vLLM,
Transformers, llama.cpp, Ollama, or TensorRT-LLM.

The compatibility check is part of the common planner/deployment contract, not
only the stub script. `PlannerRequest` carries `runtime_backend`, planner
registry dispatch validates the requested model format/backend pair, and policy
generation validates any service metadata that declares `runtimeBackend`. This
means a hand-written or repo-backed deployment policy cannot silently generate
an LLM plan that pairs a GGUF artifact with vLLM, or a TensorRT engine with a
non-TensorRT runtime.

The LLM example deliberately reuses the same DI deployment machinery as the
YOLO examples. Artifact deployment, asynchronous readiness, repo-backed
materialization, native-plan generation, and dependency references remain
framework mechanisms. LLM-specific code is limited to planner/runtime adapters:
for the current Qwen example, `ndnsf_distributed_inference.llm_runtime` provides
an OpenAI-compatible inference-payload adapter, repo-materialized llama-server
process management, and GGUF/runtime artifact materialization helpers. This
keeps `llama-server` from becoming a second, parallel deployment system.

Run the focused regression with:

```bash
python3 Experiments/NDNSF_DI_RuntimeCompatibility_Smoke.py
```

### Qwen GGUF + llama-server example

The first concrete LLM deployment example uses Qwen2.5-0.5B GGUF with
`llama-server`. This is a replicated LLM serving baseline, not transformer-layer
model parallelism: each selected provider runs or connects to a local
OpenAI-compatible llama-server, while NDNSF-DI handles service discovery,
authorization, provider selection, and deployment metadata.

This example is now treated as a secondary deployment/provisioning reference,
not the active path for context handling or model-split optimization.
`llama-server` exposes a useful OpenAI-compatible serving surface, but it does
not give NDNSF-DI a clean stage-by-stage hidden-state API. Current Qwen context
and pipeline work should therefore use the Qwen ONNX backend described below.
The GGUF/llama-server path remains useful for artifact provisioning,
asynchronous runtime readiness, and replicated-provider serving checks.

The intended deployment flow is:

```text
first setup:
  download llama-server once for each platform/architecture
  place Qwen2.5-0.5B-Instruct-Q4_K_M.gguf on a trusted deployer/repo node

NDNSF-DI deployment:
  publish/replicate model and runtime artifacts as deployment/session artifacts
  providers materialize them into local cache
  inference requests carry only OpenAI-compatible inference payloads
```

Download or inspect the platform-specific llama.cpp release asset:

```bash
python3 examples/python/NDNSF-DistributedInference/llama_server/download_llama_server.py \
  --dry-run \
  --dest third_party/llama.cpp/bin
```

Download or inspect the Qwen2.5-0.5B GGUF model artifact:

```bash
python3 examples/python/NDNSF-DistributedInference/llama_server/download_qwen_gguf.py \
  --dry-run \
  --dest third_party/qwen
```

Remove `--dry-run` from both download commands when preparing a real provider
machine. The model command downloads Qwen's `qwen2.5-0.5b-instruct-q4_k_m.gguf`
from HuggingFace; the runtime command downloads a platform-specific
`llama-server` executable from llama.cpp releases.

Generate the NDNSF-DI policy:

```bash
python3 examples/python/NDNSF-DistributedInference/llama_server/plan_llama_server.py \
  --policy /tmp/ndnsf-di-llama-server-policy.yaml \
  --model third_party/qwen/qwen2.5-0.5b-instruct-q4_k_m.gguf \
  --llama-server third_party/llama.cpp/bin/llama-server
```

Deploy the runtime executable and GGUF model into DistributedRepo, producing a
role-scoped artifact reference file:

```bash
python3 examples/python/NDNSF-DistributedInference/llama_server/deploy_artifacts.py \
  --config /tmp/ndnsf-di-llama-server-policy.yaml \
  --model third_party/qwen/qwen2.5-0.5b-instruct-q4_k_m.gguf \
  --llama-server third_party/llama.cpp/bin/llama-server \
  --out /tmp/ndnsf-di-llama-server-artifacts.json
```

On a provider node, install the repo-backed artifacts into the provider cache
and let the provider start the materialized `llama-server` automatically:

```bash
python3 examples/python/NDNSF-DistributedInference/llama_server/provider.py \
  --config /tmp/ndnsf-di-llama-server-policy.yaml \
  --artifact-references /tmp/ndnsf-di-llama-server-artifacts.json \
  --artifact-cache-dir /tmp/ndnsf-di-llama-cache \
  --llama-url http://127.0.0.1:8080
```

Model/runtime installation is intentionally asynchronous. A provider may join
the NDNSF service while the GGUF model or `llama-server` executable is still
being fetched, verified, cached, and started. Its ACK readiness probe reports
`runtimeStatus=installing` until the managed runtime is ready, so normal
selection will not choose a half-installed provider. Use
`--sync-materialize-before-serve` only for deterministic startup tests.

If a node has already installed and started `llama-server` outside NDNSF-DI, it
can omit `--artifact-references` and keep using `--llama-url` as a
pre-deployed-runtime fallback.

The user invokes the NDNSF LLM inference service with an OpenAI-compatible
payload:

```bash
python3 examples/python/NDNSF-DistributedInference/llama_server/user.py \
  --config /tmp/ndnsf-di-llama-server-policy.yaml \
  --prompt "Explain NDNSF-DI in one sentence."
```

Context-aware LLM request design is intentionally paused for this path. The
next context API should be designed around the ONNX Qwen pipeline, where token
IDs, attention masks, hidden states, and future KV-cache tensors are explicit
large-data objects. Core NDNSF already handles oversized payloads through
hybrid encrypted segmented Data and references, so the DI API should not create
a second context-transfer protocol.

The local no-MiniNDN smoke validates policy/native-plan generation, repo-backed
artifact materialization, automatic runtime launch, and the provider adapter
against a fake OpenAI-compatible server:

```bash
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case llama-server-local
```

The MiniNDN repo-backed smoke uses `AI_Lab.conf`, stores fake GGUF/runtime
artifacts in an NDNSF-DistributedRepo node, lets the provider materialize them
from the repo manifest, starts a managed fake `llama-server`, and invokes the
LLM inference service through NDNSF:

```bash
sudo -E env PYTHONPATH="$PWD/NDNSF-DistributedInference:$PWD/pythonWrapper:$PWD/Experiments:${PYTHONPATH:-}" \
  python3 Experiments/NDNSF_DI_LlamaServer_Minindn.py
```

The same smoke is also available through the DI regression runner:

```bash
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case llama-server-minindn
```

For a real Qwen2.5-0.5B GGUF + real llama.cpp `llama-server` validation run,
prepare the artifacts first:

```bash
python3 examples/python/NDNSF-DistributedInference/llama_server/download_qwen_gguf.py \
  --dest third_party/qwen

git clone --depth 1 https://github.com/ggml-org/llama.cpp.git third_party/llama.cpp-src
cmake -S third_party/llama.cpp-src -B third_party/llama.cpp-build \
  -DCMAKE_BUILD_TYPE=Release -DLLAMA_BUILD_SERVER=ON -DLLAMA_CURL=OFF -DGGML_NATIVE=OFF
cmake --build third_party/llama.cpp-build --target llama-server -j"$(nproc)"
mkdir -p third_party/llama.cpp-local/bin
cp -a third_party/llama.cpp-build/bin/llama-server third_party/llama.cpp-local/bin/
cp -a third_party/llama.cpp-build/bin/lib*.so* third_party/llama.cpp-local/bin/
```

Then run the MiniNDN real-runtime smoke:

```bash
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case llama-server-real-minindn
```

The recorded run used `AI_Lab.conf`, materialized the real Qwen GGUF model and
a self-extracting llama-server runtime bundle on the provider, then invoked the
LLM service through NDNSF-DI:

```text
LLAMA_SERVER_REAL_MININDN_OK artifact_source=local-reference \
  local_cold_ms=753.51 local_warm_ms=377.33 distributed_ms=704.77
```

For steady-state performance comparisons, use repeated requests in one user
process rather than a single smoke request:

```bash
sudo -E env PYTHONPATH="$PWD/NDNSF-DistributedInference:$PWD/pythonWrapper:$PWD/Experiments:${PYTHONPATH:-}" \
  python3 Experiments/NDNSF_DI_LlamaServer_Minindn.py \
    --real-artifacts \
    --artifact-source local-reference \
    --output-dir results/llama_server_real_minindn_simple_quiet_60s_latest \
    --model-path third_party/qwen/qwen2.5-0.5b-instruct-q4_k_m.gguf \
    --llama-runtime-dir third_party/llama.cpp-local/bin \
    --prompt 'Say hello in five words.' \
    --max-tokens 16 \
    --provider-start-timeout-s 300 \
    --timeout-ms 180000 \
    --ack-timeout-ms 1500 \
    --local-measured-requests 10 \
    --warmup-requests 2 \
    --measured-duration-s 60 \
    --request-interval-ms 1000 \
    --llama-server-extra-arg=--ctx-size \
    --llama-server-extra-arg=512 \
    --llama-server-extra-arg=--threads \
    --llama-server-extra-arg=2 \
    --llama-server-extra-arg=--parallel \
    --llama-server-extra-arg=1 \
    --llama-server-extra-arg=--no-webui
```

The 60-second run writes `llama-user-measured.csv` and
`llama-real-benchmark-summary.json`. A representative current run is
`results/llama_server_real_minindn_simple_quiet_60s_20260615_190340`:

```text
local direct llama-server p50:        351.83 ms
distributed NDNSF-DI p50:             458.68 ms
provider llama.cpp p50:               383.88 ms
estimated NDNSF/provider overhead p50: 83.21 ms
```

This is a replicated-provider LLM serving baseline, not transformer-layer
model parallelism. The provider-side llama.cpp timing still dominates the
request. NDNSF-DI adds roughly 80 ms p50 in this MiniNDN setup for service
discovery/ACK/selection, security checks, provider proxying, NFD/SVS
propagation, and Python callback dispatch. The simple predeployed-service path
is used for this single-role/no-dependency LLM service; multi-role YOLO and
future LLM pipeline plans still use the collaboration path.

The run intentionally used `artifact_source=local-reference` for the 469 MB
GGUF model. A full repo-backed ingest attempt correctly exercised the repo path
but exposed a current control-plane limitation: the model was segmented into
122,851 repo packets and did not finish within the 180 s deployment window.
That is now a separate DistributedRepo/DI provisioning task: add bulk artifact
ingest or larger native segmented storage before making multi-hundred-MB LLM
models use repo transfer in the fast regression.

### Distributed LLM pipeline validation

NDNSF-DI also includes a small LLM pipeline validation example. It is not a
real Qwen layer split yet. Instead, it proves that the same framework mechanisms
used by YOLO can drive a sequential multi-stage LLM-style plan across multiple
providers:

```text
User prompt
  -> /LLM/Pipeline/Stage/0 provider
  -> /LLM/Pipeline/Stage/1 provider
  -> /LLM/Pipeline/Stage/2 provider
  -> final response to user
```

The example reuses the common planner schema v2, role assignment, deterministic
dependency references, NDNSF discovery/selection, provider readiness, and
MiniNDN deployment. Only the stage computation is fake validation logic. This
keeps the boundary honest: the example demonstrates distributed LLM pipeline
support in NDNSF-DI, while a future Qwen/Llama/DeepSeek planner can replace the
fake stage runtime with real transformer block execution.

Run the local schema/execution smoke:

```bash
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case llm-pipeline-local
```

Run the MiniNDN distributed pipeline smoke on `AI_Lab.conf`:

```bash
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case llm-pipeline-minindn
```

Recent validation runs reported:

```text
LLM_PIPELINE_MININDN_OK local_ms=12.46 distributed_ms=251.10 stages=3
LLM_PIPELINE_MININDN_OK local_ms=5.85 distributed_ms=573.21 stages=3
```

This is a correctness smoke, not an optimized steady-state benchmark. The user
logs for these single-request runs show roughly 11-12 ms of scope-key setup and
about 238-562 ms of request/dataflow time. The spread is expected for a cold
single MiniNDN run; the next performance step is to add a warm repeated LLM
pipeline benchmark and then compare it with a real small-model stage runtime.

The next validation step replaces the fake stage computation with a real tiny
HuggingFace `LlamaForCausalLM` block pipeline. The planner writes one
`llm-stage-weights` package per role. Each package carries the stage metadata
and the subset of model weights needed by that role, and each provider preloads
its package before serving the stage. Stage outputs are serialized
hidden-state tensors and fetched by the next provider using the same
deterministic dependency reference mechanism as the fake pipeline:

```bash
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py \
  --case llm-pipeline-transformers-minindn
```

Current representative result:

```text
LLM_PIPELINE_MININDN_OK local_ms=6.49 distributed_ms=254.57 \
  stages=3 runtime=tiny-transformers
```

For a repeated MiniNDN timing check, run:

```bash
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py \
  --case llm-pipeline-transformers-benchmark
```

The benchmark keeps the same controller and providers alive, sends warmup
requests first, then reports measured p50/p95 and writes
`llm-pipeline-user-measured.csv` under the result directory. Use this case, or
the same script with `--measured-duration-s 60`, before drawing performance
conclusions from the tiny transformer pipeline.

Latest short benchmark result:

```text
LLM_PIPELINE_MININDN_BENCHMARK count=5 local_ms=30.71 avg_ms=109.72 \
  p50_ms=99.35 p95_ms=139.96 stages=3 runtime=tiny-transformers
```

This proves the NDNSF-DI pipeline can carry real transformer hidden states
across providers and produce the same final top token as local staged
execution, while using the same policy artifact mechanism that YOLO and
llama-server deployments use. It is still a tiny synthetic Llama model, not a
Qwen checkpoint split. The remaining work for a useful LLM split is
model-specific stage export, tokenizer-aware planning, KV-cache handling, a
production tensor-bundle codec for hidden states, and warm multi-request
benchmarking.

### Real Qwen local pipeline proof

The next strict proof uses a real HuggingFace Qwen checkpoint locally. It loads
`Qwen/Qwen2.5-0.5B-Instruct`, exports three stage-weight packages, executes the
packages as `layers 0-8`, `8-16`, and `16-24`, and compares the final logits
against a normal full-model forward pass:

```bash
python3 Experiments/NDNSF_DI_QwenPipeline_LocalProof.py \
  --model Qwen/Qwen2.5-0.5B-Instruct \
  --allow-download \
  --stages 3 \
  --output-dir results/qwen_pipeline_local_proof_latest
```

Recorded result:

```text
NDNSF_DI_QWEN_PIPELINE_PROOF_OK model=Qwen/Qwen2.5-0.5B-Instruct \
  stages=3 ranges=[[0, 8], [8, 16], [16, 24]] full_ms=265.75 \
  export_ms=2794.76 artifact_pipeline_ms=22360.06 max_diff=0 top_token=38444
```

This proves that a real Qwen HF model can be partitioned into stage artifacts
and re-executed stage by stage without changing the next-token result. The
large `artifact_pipeline_ms` is a local proof cost: the script reloads a stage
model for each stage to validate artifact independence. The next step is to
move these Qwen stage packages into the existing MiniNDN provider lifecycle so
each provider preloads one stage and exchanges hidden-state references through
NDNSF-DI.

The same stage packages now run through MiniNDN with one Qwen stage per
provider:

```bash
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py \
  --case llm-pipeline-qwen-minindn
```

Recorded MiniNDN result after switching providers to lightweight stage modules
that instantiate only the needed embedding/layers/norm/head components:

```text
LLM_PIPELINE_MININDN_OK local_ms=276.58 distributed_ms=1065.43 \
  stages=3 runtime=qwen-transformers
```

This is the first end-to-end Qwen pipeline proof: the user sends token IDs,
Stage 0 runs layers 0-8, Stage 1 runs layers 8-16, Stage 2 runs layers 16-24,
and the final provider returns the same expected top token (`38444`). It is a
correctness result, not an optimized performance result. The current provider
loader now uses lightweight stage modules instead of a full Qwen model
structure, but it still uses Python/Transformers execution and `torch.save`
hidden-state payloads. Future work should add a production tensor-bundle codec,
warm repeated benchmarking, and eventually a native/C++ or optimized runtime
path for stage execution.

### Provider-observed execution evidence

NativeTracer deployment summaries use `executionEvidence` as the execution
identity source. A record is created only after its runner backend/session has
initialized and binds the provider boot ID and epoch, installed roles, model
and artifact digests, plan digest, runtime version, and device. The summary's
`runnerClassification` is derived from those records. The deprecated
`runnerMode` field is compatibility output only and must not be used to claim
real model compute. Inspect a summary with:

```bash
python3 tools/ndnsf_runtime.py di evidence --summary results/.../summary.json
```

The MiniNDN candidate gate blocks missing, synthetic, mixed, contradictory, or
digest-mismatched evidence. Physical acceptance remains owned by Spec 106.

### Real Qwen ONNX pipeline example

Qwen can also be exported into multiple ONNX stages. This is similar to the
YOLO ONNX examples at the NDNSF-DI runtime level: the policy still contains
roles, artifacts, dependency edges, deterministic hidden-state names, and
large-data references. The splitter is model-specific, however. YOLO uses a
YOLO/ONNX graph splitter, while Qwen uses a transformer layer-range splitter
that exports one ONNX stage per contiguous decoder-layer range.

This is the active Qwen direction. Context support should extend this ONNX
pipeline contract rather than the llama-server/GGUF baseline. The near-term
shape is:

```text
current turn input:
  token_ids
  attention_mask
  optional position_ids

reusable context state:
  previous token ids or encoded prompt reference
  future KV-cache tensor references
  session id / cache epoch metadata

stage exchange:
  deterministic hidden-state Data names
  explicit tensor bundle metadata
  NDNSF large-data references when payloads exceed one segment
```

The initial implementation should remain conservative: support full-context
token/attention tensors first, then add append-only context deltas and KV-cache
reuse once the ONNX stage runtime exposes the required tensors cleanly.
The current API object is `ndnsf-di-qwen-pipeline-context-v1`: it carries
`inputIds`, `attentionMask`, optional `positionIds`, `sessionId`, and
`contextEpoch`. Small contexts may be sent inline. Large contexts should be
published through the existing NDNSF large-data path, and the request carries
only the standard `LargeDataReference` payload. This keeps context semantics in
NDNSF-DI while leaving segmentation, hybrid encryption, digest checking, and
fetching to NDNSF Core.

The same schema also reserves `contextMode=append-delta` for append-only
updates. A delta request carries `sessionId`, `baseContextEpoch`,
`contextEpoch`, and `delta.inputIds`/`delta.attentionMask`. Stage 0 expands the
delta against its cached full context and rejects the request if the session is
missing or the epoch does not match. The current regression mode sends an empty
delta after the first full context so the cache/epoch path can be tested without
changing the expected token output. Future KV-cache reuse should use the
existing `kvCacheReference` field with explicit cache epoch and invalidation
rules; it should not invent another context transport path.

#### KV-cache reference lifecycle

`kvCacheReference` is a performance hint for future Qwen ONNX pipeline
runtimes, not a replacement for `inputIds`/`attentionMask` correctness. The
cache owner is the provider role that produced the cached tensors. In the
current layer pipeline this normally means Stage 0 owns prompt/input embedding
state, while later stages may own per-stage hidden/KV tensors once their ONNX
artifacts expose them. A cache entry is scoped by:

```text
sessionId
stage role
contextEpoch
model artifact hash
planner/native-plan hash
security scope or key epoch
```

Epochs are monotonic within one `sessionId`. A full-context request at epoch
`E` installs or replaces the cache for that epoch. An append-delta request must
name `baseContextEpoch=E` and `contextEpoch=E+1` or higher. Providers must
reject stale, skipped, or conflicting epochs unless the request also carries a
full-context fallback payload that can rebuild the cache deterministically.

Cache miss fallback is explicit:

```text
1. If kvCacheReference is present and valid, use it.
2. If it is missing/stale/evicted but the request carries full inputIds and
   attentionMask, rebuild from full context and install the new epoch.
3. If the request is delta-only and the cache is missing or stale, fail with a
   cache-miss status so the user can resend a full-context request.
```

Provider cache eviction is local policy. A provider may evict by TTL, memory
pressure, maximum sessions, maximum bytes, artifact update, key epoch change,
or provider restart. Eviction must not change wire security semantics: cached
tensors are derived from request data and must be scoped to the same service,
security epoch, model artifact, and provider role. Repo-backed or remote
KV-cache storage is not part of the first design; if added later, it should use
the same large-data reference and digest rules as activation objects.

The current example exports three Qwen ONNX stages:

```text
Stage 0: token IDs -> embedding -> layers 0-8 -> hidden-state npz
Stage 1: hidden-state npz -> layers 8-16 -> hidden-state npz
Stage 2: hidden-state npz -> layers 16-24 -> logits/top token
```

The MiniNDN script supports this runtime with `--runtime qwen-onnx`:

```bash
sudo -n python3 Experiments/NDNSF_DI_LlmPipeline_Minindn.py \
  --runtime qwen-onnx \
  --output-dir results/qwen_onnx_pipeline_minindn_smoke2 \
  --topology-file Experiments/Topology/AI_Lab.conf \
  --warmup-requests 3 \
  --measured-duration-s 60 \
  --request-interval-ms 200
```

To force the full-context input through the standard large-data/reference path:

```bash
sudo -n python3 Experiments/NDNSF_DI_LlmPipeline_Minindn.py \
  --runtime qwen-onnx \
  --reuse-existing-policy \
  --output-dir results/qwen_onnx_pipeline_minindn_smoke2 \
  --topology-file Experiments/Topology/AI_Lab.conf \
  --measured-requests 1 \
  --publish-input-reference
```

To validate append-only context-delta handling while preserving the expected
top token:

```bash
sudo -n python3 Experiments/NDNSF_DI_LlmPipeline_Minindn.py \
  --runtime qwen-onnx \
  --reuse-existing-policy \
  --output-dir results/qwen_onnx_pipeline_minindn_smoke2 \
  --topology-file Experiments/Topology/AI_Lab.conf \
  --measured-requests 2 \
  --context-input-mode append-empty-delta-after-first \
  --publish-input-reference
```

To validate real append-delta correctness, append one or more token IDs and
compare the distributed result with a locally recomputed full-context ONNX
pipeline result:

```bash
sudo -n python3 Experiments/NDNSF_DI_LlmPipeline_Minindn.py \
  --runtime qwen-onnx \
  --reuse-existing-policy \
  --output-dir results/qwen_onnx_pipeline_minindn_smoke2 \
  --topology-file Experiments/Topology/AI_Lab.conf \
  --measured-requests 2 \
  --context-input-mode append-token-delta-after-first \
  --delta-token-ids 2 \
  --publish-input-reference
```

The same regression is wired into the DI regression suite and the optional
MiniNDN quick-suite case:

```bash
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py \
  --case llm-pipeline-qwen-onnx-delta-minindn

python3 Experiments/NDNSF_Run_Minindn_Quick_Checks.py \
  --case di-llm-qwen-onnx-delta-minindn
```

Recorded 60-second result on `AI_Lab.conf`:

```text
LLM_PIPELINE_MININDN_BENCHMARK count=119 local_ms=268.44 \
  avg_ms=289.02 p50_ms=287.14 p95_ms=358.52 stages=3 runtime=qwen-onnx
LLM_PIPELINE_QWEN_PROFILE_STAGE stage=0 compute_p50_ms=31.80 total_p50_ms=38.15
LLM_PIPELINE_QWEN_PROFILE_STAGE stage=1 compute_p50_ms=45.94 fetch_p50_ms=56.07 total_p50_ms=115.32
LLM_PIPELINE_QWEN_PROFILE_STAGE stage=2 compute_p50_ms=66.59 fetch_p50_ms=131.34 total_p50_ms=206.44
```

This proves that the Qwen ONNX model can be split into stage artifacts and run
across MiniNDN providers through the same NDNSF-DI dependency path used by YOLO.
It is still a pipeline-parallel proof for one next-token forward pass; it does
not yet include KV-cache-aware decode, tensor-parallel attention/MLP sharding,
or an optimized native C++ provider for the Qwen ONNX stages.

The client can publish the manifest through NDNSF:

```python
client = APPClient.from_config("yolo_policy.yaml")
result = client.publish_service_manifest("/AI/YOLO/SplitInference")
print(result.encrypted_data_name)
```

This uses the same NDNSF encrypted large-Data path as model shards and runtime
artifacts: the payload is carried in NDN Data packets signed by the local NDN
identity, and encrypted when the service policy requires confidentiality. Model
files, ONNX shards, runner scripts, and executable bundles follow the same
rule: they are artifacts only after being published as signed NDN Data.

The role scripts call `load_or_generate_deployment()` automatically, so the
checked-in YOLO example can be run directly from the high-level config.

Executable artifacts are disabled by default. To allow downloaded binaries or
scripts to be marked executable, a deployment must configure all of:

```yaml
trust:
  anchor_file: /path/to/root.cert

artifact_security:
  allowlist:
    - /NDNSF/Runtime/TrustedBackend/v1
  sandbox:
    kind: container
    image: registry.example/ndnsf-runtime:latest
```

If any of these are missing, provider code that asks for
`allow_executables=True` fails before the provider starts serving requests.

## YOLO ONNX Split Example

The example exports a small Ultralytics YOLO model into two ONNX stages. The
splitter writes the ONNX shard paths into the generated policy, providers load
their local shard for their assigned role, and the user requests the service by
name.

Install Python dependencies:

```bash
python3 -m pip install -e ./pythonWrapper
python3 -m pip install -e ./NDNSF-DistributedInference
python3 -m pip install "ultralytics>=8.4" "onnx>=1.16" "onnxruntime>=1.18"
```

Generate ONNX shards and a policy from the YOLO splitter:

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_split/split_model.py \
  --model yolo26n.pt \
  --auto-split \
  --out-dir /tmp/ndnsf-yolo-split \
  --policy /tmp/ndnsf-yolo-split/yolo_policy.yaml
```

With `--auto-split`, the splitter first exports a full ONNX model, runs the
optional graph analyzer and homogeneous-provider planner, maps the recommended
ONNX cut back to a YOLO module boundary, and then exports the two ONNX stages.
Without `--auto-split`, the example keeps the fixed YOLO-specific split for
repeatability. Both paths still emit the same `SplitterOutput` policy format.

After installation, application code can import the distributed inference layer
from any working directory:

```python
from ndnsf_distributed_inference import DistributedInferenceClient
```

Run one role per shell or MiniNDN node:

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_split/controller.py \
  --config /tmp/ndnsf-yolo-split/yolo_policy.yaml
python3 examples/python/NDNSF-DistributedInference/yolo_split/provider.py \
  --config /tmp/ndnsf-yolo-split/yolo_policy.yaml \
  --temp-dir /tmp/ndnsf-yolo-stage0
python3 examples/python/NDNSF-DistributedInference/yolo_split/provider.py \
  --config /tmp/ndnsf-yolo-split/yolo_policy.yaml \
  --provider-id A --temp-dir /tmp/ndnsf-yolo-stage1
python3 examples/python/NDNSF-DistributedInference/yolo_split/user.py \
  --config /tmp/ndnsf-yolo-split/yolo_policy.yaml
```

For an end-to-end MiniNDN regression that generates the auto-split policy and
then runs the controller, Stage 0 provider, Stage 1 provider, and user on
separate MiniNDN nodes:

```bash
sudo -E python3 Experiments/NDNSF_DI_YoloSplit_Minindn.py
```

The smoke test succeeds only when the user log contains:

```text
YOLO_SPLIT_RESULT ... ok=true
YOLO_SPLIT_MININDN_OK ...
```

The same smoke test is also available through the unified DI regression entry:

```bash
sudo -E python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case auto-split
```

The unified entry also includes a fast local ONNX executor check that does not
start MiniNDN:

```bash
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case onnx-executor
```

## YOLO Layout Split API Example

The `yolo_2x2` example is now explicitly a YOLO sequential-chunk regression.
Its historical default is 2x2: two pipeline stages represented by two
sequential chunks per stage. The same splitter accepts custom chunk layouts
such as `1x3`, `2x3`, `3x2`, and `3x3`. It uses the real Ultralytics YOLO nano
model and exports one ONNX chunk per generated role, but it does not implement
parallel shards within the same stage.

For true parallel-graph experimentation, prefer
`split_model.py --parallel-detect-scale-shards`. It generates a shared
`/Backbone` role, parallel YOLO Detect scale head roles, and a `/Merge` decode
role. This is a model-specific YOLO DAG rather than a generic rectangular
`N x M` mapper, but it avoids duplicating backbone compute and is the current
closest example of real parallel model execution in NDNSF-DI. Its generated
policy and `native-execution-plan.json` keep every fan-in edge scope unique, so
`/Merge` receives one planned input per head shard and can batch-wait for all
required inputs.

For the 640-pixel YOLO experiment, the Detect-scale shard mode now uses a
candidate-filter boundary instead of sending raw Detect head tensors to
`/Merge`. Each `/Head/Shard/*` role decodes the boxes for its assigned YOLO
Detect scales, keeps the top `max_det` candidate anchors by class score, and
publishes one `candidates_shard_*` tensor. `/Merge` concatenates those candidate
tensors and runs the global YOLO top-k step. This preserves the full-model
postprocess result within ONNX floating-point tolerance while reducing the
Head-to-Merge activation from multi-megabyte raw box/score tensors to about
101 KB, or 15 planned segments, per head edge at 640. It does not solve the
larger Backbone-to-Head feature transfer: in the current 2x2 plan those edges
are still roughly 2.46 MB and 410 KB, so the next splitter work should move the
cut later, colocate backbone and head work when possible, or score candidate
plans by activation bytes, compute saved, and transfer cost.

The older `split_model.py --parallel-output-shards` mode remains available as a
minimal fan-in correctness scaffold. It should not be used as a performance
claim because its Stage-0 shards duplicate upstream YOLO compute.

The dependency edges in `yolo_policy.yaml` are generated from the exported ONNX
chunk IO, not from hardcoded topic names alone. For the default 2x2 split, the
cross-role tensor sets are:

```text
/Stage/0/Shard/0 -> /Stage/0/Shard/1: x
/Stage/0/Shard/1 -> /Stage/1/Shard/0: x, saved_4
/Stage/1/Shard/0 -> /Stage/1/Shard/1: x, saved_4, saved_10, saved_13
```

Each edge publishes one activation large object containing the listed tensors,
and the consumer fetches that object before continuing its ONNX chunk. This is
a chunk-level collaboration graph derived from real model tensor boundaries;
the internal YOLO operator graph still runs locally inside each chunk.
The generated `*-<layout>-onnx-graph-summary.json` also records full-model candidate
split points, so later planners can compare different cut positions without
changing the NDNSF-DI policy interface.

Within one distributed-inference run, providers do not call each other through
new NDNSF service invocations. The outer user request starts the run and assigns
roles. After that point, each provider waits for the input edges declared in the
dependency graph, fetches the corresponding activation large-data object when
the reference appears, runs its local ONNX chunk, and publishes output-edge
activation references for downstream roles. The provider-to-provider part of
the run is therefore dataflow-driven, not a chain of Request/ACK/Selection
service calls.

Because a compiled distributed-inference plan makes dependency scopes, topic
prefixes, producer roles, and consumer roles predictable, providers can also
prefetch planned inputs. `ProviderRuntimeContext.prefetch_input_large(...)`
starts a background wait/fetch for a role-local dependency reference, and
`wait_prefetched_input_large(...)` returns the fetched activation object when
the handler needs it. This optimization is generic: it depends only on the
declared dependency edge and topic suffix, not on YOLO. It should be used only
when the plan gives deterministic dependency topics; otherwise handlers can keep
using explicit `wait_one(...)` and `fetch_large_reference(...)`. New code should
publish dependency references with the standard NDNSF large-data reference
payload instead of placing naked Data names in collaboration messages.

The current prefetch path begins as soon as the provider handler starts. It can
overlap local preparation with waiting for upstream activation objects. For
planned ONNX dependencies, the policy may include `object_name_template`,
`expected_segments`, and `expected_bytes`. The template is filled with the
current run/session id, key scope, producer role, producer provider, and bundle
sequence. Producers publish activation objects at that deterministic name. For
static-shape edges, DI treats the segment count as part of the execution plan:
consumers expand the object name into planned segment Data names and pre-issue
all segment Interests as soon as the role starts. For dynamic-shape edges, the
consumer starts with segment 0 and follows the final block id. This is the
intended dataflow optimization: dependency traffic remains NDN large Data, not
provider-to-provider service invocation, and the application no longer waits
for a separate activation-reference control message before fetching planned
inputs.
The NDNSF core provider path also keeps pending Interests for IMS-served Data.
Therefore a downstream role may issue an Interest for a deterministic
activation name before the upstream role has finished publishing it; if the
Interest is still within its lifetime, the upstream provider replies as soon as
the activation segments are inserted. This is why DI uses a formal
`object_name_template` and segment-count hint in the policy instead of letting
applications privately guess Data names. The generated `native-execution-plan`
also records `segmentNaming` metadata so deployment artifacts can be reviewed
without reading runtime code.
There is no separate activation-ready notification in the native dependency
I/O path.  A consumer learns that an upstream activation exists by having an
already pending Interest satisfied, or by retrying/falling back through normal
NDN segmented retrieval when the edge is dynamic.
The same reference metadata is attached to repo-backed model/runtime artifacts
inside execution specs, even though their bytes are fetched through the repo's
manifest-aware object API.
Provider artifact materialization now checks that reference first and then
falls back to legacy `repoManifest`, chunk-list, or single Data-name fields.

For ONNX chunks, the helper
`execute_onnx_dependency_chunk(...)` is the preferred provider-side path. It
uses the current role's dependency view to collect all input-edge tensor
bundles, merge them by tensor name, run the assigned ONNX chunk, and publish
one tensor bundle for each declared output edge. The YOLO provider now uses
this dependency-driven executor and decides whether a role is the first,
intermediate, or final chunk from the role-local dependency view. The
YOLO-specific code only prepares the first image input and encodes the final
prediction response. This lets the same deployed provider logic run `1x3`,
`2x3`, `3x2`, `3x3`, and the default `2x2` layout.

The executor caches ONNX Runtime sessions by model size and SHA-256 digest
inside each provider process. Repeated role executions can reuse the same
session even when artifacts are materialized into fresh temporary paths, while
still noticing a regenerated model file. This keeps the public APP API
unchanged and reduces repeated per-request session initialization overhead.

The client also caches published plan-level references in one process. For the
same plan fingerprint, repeated inference requests reuse artifact-spec and
scope-key large-data references instead of publishing the same model/runtime
metadata again. Only the per-request input reference is published for each
inference. Long-running services should eventually add explicit plan-session
rotation, but the default APP API remains unchanged.

For performance analysis, the executor logs:

```text
NDNSF_DI_ONNX_TIMING
NDNSF_DI_DEPENDENCY_INPUT_TIMING
NDNSF_DI_DEPENDENCY_OUTPUT_TIMING
NDNSF_DI_PLAN_CACHE
```

These lines split latency into input collection, activation reference wait,
large-object fetch, tensor decode, ONNX session lookup, ONNX run, and output
publish time. The dependency input/output timing lines include the DI session
identifier, so a long run can be grouped by individual inference request rather
than mixing cold and warm paths. They also report actual payload bytes plus
planned segment and byte counts when the splitter can estimate them. They are
meant to guide generic dataflow optimizations instead of tuning a single YOLO
layout by hand.

The executor also has a small non-MiniNDN smoke test that builds a toy ONNX DAG
with one fan-out edge and one fan-in join:

```bash
PYTHONPATH="NDNSF-DistributedInference:$PYTHONPATH" \
  python3 Experiments/NDNSF_DI_OnnxExecutor_Smoke.py
```

The smoke succeeds only when it prints:

```text
ONNX_EXECUTOR_FANIN_FANOUT_OK
```

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/split_model.py \
  --model yolo26n.pt \
  --input-size 32 \
  --layout 3x2 \
  --auto-split \
  --out-dir /tmp/ndnsf-yolo-3x2 \
  --policy /tmp/ndnsf-yolo-3x2/yolo_policy.yaml
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/provider.py \
  --config /tmp/ndnsf-yolo-3x2/yolo_policy.yaml --provider-id A --roles all
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/provider.py \
  --config /tmp/ndnsf-yolo-3x2/yolo_policy.yaml --provider-id B --roles all
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/provider.py \
  --config /tmp/ndnsf-yolo-3x2/yolo_policy.yaml --provider-id C --roles all
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/provider.py \
  --config /tmp/ndnsf-yolo-3x2/yolo_policy.yaml --provider-id D --roles all
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/user.py \
  --config /tmp/ndnsf-yolo-3x2/yolo_policy.yaml
```

With `--auto-split`, the splitter uses the ONNX planner recommendation as a
pipeline-boundary hint; the requested layout determines how many chunks are
exported. Without it, the example keeps the previous YOLO-specific split hint
as a stable fallback.

For MiniNDN, run:

```bash
sudo -n python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py
```

Expected output includes:

```text
YOLO_2X2_RESULT ... ok=true
YOLO_2X2_DYNAMIC_PROVISIONING_MININDN_OK ...
```

The same script also writes regression statistics under
`results/yolo_<layout>_minindn_quick/`:

```text
inference-latency-stats.json
traffic-stats.json
nfd-data-stats.json
plan-cache-stats.json
onnx-timing-stats.json
dependency-input-timing-stats.json
dependency-output-timing-stats.json
dependency-volume-stats.json
dependency-frontier-timing-stats.json
```

These files record end-to-end latency, node traffic counters, NFD Data counters,
plan-cache hits, ONNX session/run time, per-edge activation reference/fetch
timing, per-edge activation publish timing, planned-vs-actual activation
volume, and producer-output-ready to consumer-first-segment frontier timing.
Use them to decide whether the next
bottleneck is ACK/selection, artifact publication, ONNX execution, activation
reference wait, segmented fetch, activation publish, frontier scheduling, or
tensor decoding.

AI-oriented MiniNDN regressions use
`Experiments/Topology/AI_Lab.conf` by default. That topology uses five nodes:
`memphis` as the user/controller-side hub and four directly attached 1 ms,
1 Gbps provider nodes (`ucla`, `arizona`, `wustl`, `neu`). It is intentionally
smaller than the older `AI_testbed.conf` so MiniNDN/NFD CPU scheduling noise is
lower during DI performance experiments. Use `AI_testbed.conf` only when a
larger multi-node topology is the point of the experiment. Do not set a global
`NDN_LOG` value around the MiniNDN command: NFD inherits that environment and
can fail to start on application-style logging filters. For low-noise benchmark
runs, use the script flags and `NDNSF_TIMELINE_TRACE_SAMPLE_RATE` shown below.

The current 2x2 native-provider benchmark recipe is:

During this MiniNDN setup, each DI user/provider/controller identity receives
two root-signed keys in its per-node `/tmp/Minindn/<node>` keychain: an RSA key
kept as the default encryption certificate for NAC-ABE/PermissionResponse
unwrap, and an ECDSA key used as the optional signing certificate when present.
The runtime chooses and caches these certificate roles at process startup; it
does not rescan the keychain for every request or Data packet. If the ECDSA key
is absent at startup, NDNSF falls back to signing with the RSA certificate until
that process is restarted.
Use `--single-rsa-certs` only as a control run when comparing the dual-certificate
signing path against the older RSA-only behavior.

```bash
sudo mn -c >/tmp/ndnsf_mn_cleanup.log 2>&1 || true
sleep 3
sudo -E env NDNSF_TIMELINE_TRACE_SAMPLE_RATE=1 \
  python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py \
  --layout 2x2 \
  --parallel-detect-scale-shards \
  --native-providers \
  --cold-requests 1 \
  --warm-duration-s 60 \
  --warm-interval-ms 1000 \
  --preflight-requests 2 \
  --ack-timeout-ms 300 \
  --timeout-ms 10000 \
  --quiet-perf-logs \
  --results-dir results/yolo_2x2_native_provider_selection_direct_prefetch_60s_minimal
```

The representative run in
`results/yolo_2x2_native_provider_selection_direct_prefetch_60s_minimal` produced
`YOLO_2X2_NATIVE_PROVIDERS_MININDN_OK`, 60 measured warm requests, warm p50
66.91 ms, warm p95 95.61 ms, and warm mean 67.02 ms. After discarding the
first measured warm request, steady warm p50 was 66.76 ms, p95 was 95.64 ms,
and mean was 66.62 ms. NFD observed 1212 incoming/outgoing Data packets across
the 62 warm-side requests counted by the script, or 19.55 Data packets per
observed request. The measured-window ratio was 20.20 NFD out Data packets per
inference and about 451 KB of NFD `nOutBytes` per measured inference. As above,
the byte average is an approximate transport-size ratio because NFD byte
counters include all packet types on the measured faces.

For the 640-pixel YOLO native-provider experiment, use the same AI_Lab topology
and the same `--parallel-detect-scale-shards --native-providers` path, but pass
`--model yolo26n.pt --input-size 640`. The current candidate-filter split was
validated locally against the full exported ONNX model with max absolute
difference about `5.8e-4`, and its local cached chunk benchmark produced about
160.75 ms p50 versus about 88.97 ms p50 for the full local ONNX graph. A
representative 60-second MiniNDN run in
`results/yolo26n_640_2x2_candidate_split_60s_latest` produced
`YOLO_2X2_NATIVE_PROVIDERS_MININDN_OK`, warm p50 5331.88 ms, p95 10086.67 ms,
about 956 NFD out Data packets per observed request, and about 7.13 MB of
NFD out bytes per observed request. This is not yet a good distributed speedup
result; it is evidence that the Head-to-Merge candidate-filter boundary works,
while the remaining Backbone-to-Head feature transfer and MiniNDN/NFD transport
cost dominate the 640 run.

The replicated-backbone Detect plan is the current 640-pixel counterpoint. It
uses `--parallel-detect-replicated-backbone-shards --native-providers` so each
Head shard repeats the backbone/neck work locally and only sends candidate
tensors to `/Merge`. Local split verification for `yolo26n.pt --input-size 640`
matched the full ONNX graph within about `5.8e-4`; the local cached chunk
benchmark produced 231.46 ms p50 because it repeats backbone work. The network
benefit is much larger: `results/yolo26n_640_2x2_replicated_backbone_60s_latest`
produced `YOLO_2X2_NATIVE_PROVIDERS_MININDN_OK` for 60 measured warm requests,
warm p50 273.49 ms, p95 349.53 ms, about 88.79 NFD out Data packets per
observed request, and about 681.9 KB of NFD out bytes per observed request.
This does not prove replicated-backbone is always optimal; it shows that for
the current AI_Lab topology and YOLO 640 profile, reducing cross-node activation
bytes is more valuable than avoiding duplicated backbone compute.

The splitter now prints and records `YOLO_LAYOUT_PLANNER_COST`,
`YOLO_LAYOUT_PLANNER_DOMINANT_EDGE`, `YOLO_LAYOUT_PLANNER_EDGE_COST`,
`YOLO_LAYOUT_PLANNER_COMPUTE`, and `YOLO_LAYOUT_PLANNER_CANDIDATE` lines.
These are planner-time hard metrics, not runtime guesses: each dependency edge
reports expected activation bytes, planned segment count, and a coarse transfer
estimate from the provider profile, while the compute lines report export-time
role forward timings as a relative signal. On the current AI_Lab default profile
(1 Gbps links and about 4 ms provider-to-provider RTT through `memphis`), the
640 candidate-filter plan reports about 3.07 MB and 441 planned segments across
four activation edges. The dominant edge is `backbone-to-head-shard0`, about
2.46 MB and 352 planned segments, with a coarse transfer estimate of about
23.67 ms before NFD scheduling, validation, retries, and application dispatch.
Use these cost lines as the first filter when trying a new YOLO split: a plan
that saves compute but introduces a multi-megabyte cross-node activation should
not be considered a good DI plan unless the saved compute clearly dominates the
transfer cost.

The first auto-planner smoke uses
`results/yolo_2x2_auto_parallel_detect_smoke_latest` and the AI_Lab topology.
For 32-pixel input it selected replicated-backbone in that run, verified the
native provider MiniNDN path, and produced `YOLO_2X2_NATIVE_PROVIDERS_MININDN_OK`
with three warm requests at p50 62.54 ms. For 640-pixel input, the local planner
probe selected replicated-backbone because the shared candidate estimated about
3.07 MB / 441 planned segments while the replicated candidate estimated about
202 KB / 30 planned segments.

The current native provider path uses deterministic activation names,
active-put segment delivery, and direct Selection prefetch by default in the DI
MiniNDN native-provider experiment. `NDNSF_COLLAB_LARGE_ACTIVE_PUT=1` keeps
generated activation segments in IMS and also immediately pushes them to NFD so
pre-issued exact segment Interests can be satisfied without waiting for an
additional application-level notification path. `NDNSF_SELECTION_TARGETED_PREFETCH=1`
lets each provider pre-express an Interest for the predictable SelectionMessage
name after it publishes an ACK; the user still publishes the same Selection via
SVS, but also puts a signed Data packet under the same selection name. Providers
feed that Data into the same selection handler, token checks, and hybrid
decrypt path, while the later SVS duplicate is suppressed by the normal
duplicate guard. Core keeps this feature behind the environment variable; the
DI native-provider runner enables it for this measured experiment. Disable
these behaviors only for A/B diagnosis with
`NDNSF_COLLAB_LARGE_ACTIVE_PUT=0` or `NDNSF_SELECTION_TARGETED_PREFETCH=0`.

This minimal recipe intentionally disables detailed dependency/control/crypto
timing so the latency number is not dominated by tracing. Treat this 60-second
shape as the performance baseline. For bottleneck analysis, keep the same
60-second measured window and add only narrow sampled instrumentation such as
`--control-timing`, `--dependency-timing`, or `--crypto-timing`; those diagnostic
runs explain the remaining outer ACK/Selection/Response and SVS delivery cost,
but should not be compared directly with the minimal latency baseline.

A sampled control-timing diagnostic run is preserved in
`results/yolo_2x2_native_provider_selection_direct_prefetch_60s_control`. It
used the same 60-second shape with `--control-timing` and produced 60 measured
warm requests with p50 78.72 ms, p95 96.92 ms, and steady-after-first p50
78.64 ms. The run measured p50 request latency at 78.66 ms, provider dataflow
at 20.00 ms, role run window at 16.00 ms, ONNX run sum at 2.52 ms, and
outer-control residual at 51.93 ms. Activation reference wait was 0.00 ms for
all planned inputs, all 252 native dataflow inputs and outputs matched the
deterministic plan, and activation publish p50 was about 1.21 ms per edge.
Control propagation is still visible: request SVS to provider request
observation was about 18.02 ms p50, ACK SVS to user pre-decrypt about 7.62 ms
p50, selection delivery to provider selection handling about 15.73 ms p50 in
the sampled rows, and response SVS to user observation about 15.69 ms p50.
Provider-side request admission, provider-side selection decrypt/dispatch, and
response publication remain sub-millisecond. In other words, ONNX and
activation publication are no longer the bottleneck; the largest remaining
residual is the outer ACK/Selection/Response control path and NFD/SVS delivery
around request, ACK, selection, and response.

Control-timing runs also print `YOLO_LAYOUT_SELECTION_DIRECT_PREFETCH` and write
`selection-direct-prefetch-stats.json`. That diagnostic pairs the user-side
`SELECTION_DIRECT_PUT` event with each provider-side
`SELECTION_DIRECT_PREFETCH_DATA` and `SELECTION_OBSERVED` event. Use it to
answer the narrow question of whether direct Selection Data is stuck between
`Face::put()` and provider handling. A short packet-trace smoke at
`results/yolo_2x2_selection_direct_put_packet_trace_smoke` showed the first
large NDN/UDP packets on the user node about 3 ms after `SELECTION_DIRECT_PUT`,
and provider nodes observed corresponding traffic within a few more
milliseconds. This points away from a tens-of-milliseconds `Face::put()` stall
on the direct Selection path.

If the question is whether ndn-svs itself is spending milliseconds inside
Sync processing, use `--svs-internal-timing` for a short diagnostic run. This
enables only `ndn_svs.SyncTimeline` and `ndn_svs.SVSPubSub` TRACE logs and
writes `svs-internal-timing-stats.json`; it is not a baseline benchmark mode.
A short smoke at `results/yolo_2x2_svs_mapping_timing_smoke` showed
`sync_worker_p50_ms=0.000`, `sync_main_blocked_p50_ms=0.000`,
`sync_encode_p50_us=66.0`, `sync_sign_p50_us=702.5`, and
`sync_face_put_p50_us=11.0`. Extra mapping piggyback was also bounded:
34 extra-block builds carried 28 mapping entries and 24 piggyback Data packets
in total, with p50 extra-block size about 1145 bytes and no network mapping
fetches. The mapping-specific split is even smaller: mapping block construction
had `extra_mapping_build_total_p50_us=16.5`, while receive-side mapping parse
and processing had `extra_mapping_parse_total_p50_us=244.5` and
`extra_mapping_parse_process_p50_us=157.5`. In other words, the current
evidence does not support a large per-message ndn-svs CPU bottleneck or
repeated full-history mapping payload. The remaining cost is primarily the
number of outer control deliveries Request -> ACK -> Selection -> Response
over the measured MiniNDN/NFD/SVS path.

For activation transport, dependency-timing runs also print
`YOLO_LAYOUT_ACTIVATION_SEGMENT_TIMELINE` and write
`activation-segment-timeline-stats.json`. That diagnostic pairs consumer
`segment_interest`, producer `segment_active_put`, and consumer
`segment_received/segment_validated` events by deterministic segment name. A
run also writes `dependency-edge-ndnping-rtt-stats.json`, which measures
ndnping RTT along each planned dependency edge from the consumer provider node
to the producer provider prefix. Use this edge RTT, not only user-to-provider
RTT, when explaining activation delivery; in AI_Lab, the activation edges can
have materially higher observed RTT than the memphis-to-provider baseline. A
short diagnostic smoke at `results/yolo_2x2_activation_segment_timeline_smoke`
showed `interest_to_data_p50_ms=83.81`, but
`interest_to_active_put_p50_ms=80.27` and `active_put_to_data_p50_ms=8.51`.
Thus the large apparent activation fetch time is mostly productive prefetch
wait for the upstream role to finish and publish its segment. Once the producer
active-puts the Data, delivery and validation are much smaller. This
distinction matters when interpreting dependency fetch p50: prefetch overlap is
not the same as blocking network transfer.

A second control run with synchronous local SVS publish is preserved in
`results/yolo_2x2_native_provider_active_put_60s_sync_publish_control`. It
produced warm p50 95.28 ms and p95 115.27 ms, reducing outer-control residual
from 78.56 ms to 73.59 ms. This is a useful A/B result but not a complete fix:
selection SVS to provider observation remained about 33.67 ms p50. Therefore,
do not keep compressing SelectionMessage payloads or changing ndn-svs timing by
default. That result motivated the direct Selection prefetch path described
above: keep the same selection name, permissions, tokens, and hybrid encryption
semantics, but let providers prefetch the predictable Selection Data directly.

When the question is specifically whether the delay is in NFD/SVS packet
propagation, first run the narrow 60-second control diagnostic without packet
capture:

```bash
sudo mn -c >/tmp/ndnsf_mn_cleanup.log 2>&1 || true
sleep 3
sudo -E env NDNSF_TIMELINE_TRACE_SAMPLE_RATE=10 \
  python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py \
  --layout 2x2 \
  --parallel-detect-scale-shards \
  --native-providers \
  --cold-requests 1 \
  --warm-duration-s 60 \
  --warm-interval-ms 1000 \
  --preflight-requests 2 \
  --ack-timeout-ms 300 \
  --timeout-ms 10000 \
  --quiet-perf-logs \
  --control-timing \
  --results-dir results/yolo_2x2_native_provider_selection_direct_prefetch_60s_control
```

If the question is whether a 40-100 ms latency swing correlates with MiniNDN/NFD
forwarding pressure, keep the same 60-second measured window and add only the
warm RTT/NFD monitor:

```bash
sudo mn -c >/tmp/ndnsf_mn_cleanup.log 2>&1 || true
sleep 3
sudo -E env NDNSF_TIMELINE_TRACE_SAMPLE_RATE=1 \
  python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py \
  --layout 2x2 \
  --parallel-detect-scale-shards \
  --native-providers \
  --cold-requests 1 \
  --warm-duration-s 60 \
  --warm-interval-ms 1000 \
  --preflight-requests 3 \
  --ack-timeout-ms 300 \
  --timeout-ms 10000 \
  --quiet-perf-logs \
  --control-timing \
  --warm-rtt-monitor-interval-s 1 \
  --results-dir results/yolo_2x2_warm_rtt_nfd_monitor_60s
```

This writes `warm-rtt-nfd-monitor.json`. Each measured inference result carries
`epoch_start_s` and `epoch_end_s`; the monitor aligns each request with the
nearest user-to-provider ndnping-style RTT sample and NFD network-face Data
counter delta. This mode adds four ndnping probes per sample interval, so use
it for correlation and diagnosis, not as the canonical low-overhead benchmark.
With `--control-timing`, the script also writes
`outer-control-rtt-correlation-stats.json`, which aligns each request with
outer ACK/Selection/Response timing and reports the strongest correlations with
inference latency. Use `NDNSF_TIMELINE_TRACE_SAMPLE_RATE=1` for this correlation
run; sampled rates such as `10` are useful for lower overhead summaries but do
not provide one control row per inference request.
The same run also writes `native-session-breakdown-stats.json`, which joins the
user outer-control row with provider role timing, dependency fetch timing, and
ONNX timing by request/session id. Use that file for individual outliers: it
separates request-to-first-ACK delay, ACK-to-selection delay, selection-to-final
response delay, final merge execution, and the slowest activation dependency
fetch. In the current 2x2 native-provider diagnostics, RTT/NFD counter drift is
weakly correlated with latency; the larger swings are usually outer
ACK/Selection propagation or final merge activation fetch waits.

When comparing RSA-only signing with RSA+ECDSA split certificates, do not use
separate MiniNDN runs as the primary evidence. MiniNDN/NFD RTT drift can be
larger than the signing difference. Use one topology and run the signing modes
back-to-back:

```bash
sudo mn -c >/tmp/ndnsf_mn_cleanup.log 2>&1 || true
sleep 3
sudo -E env NDNSF_TIMELINE_TRACE_SAMPLE_RATE=0 \
  python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py \
  --layout 2x2 \
  --parallel-detect-scale-shards \
  --native-providers \
  --cold-requests 1 \
  --warm-duration-s 60 \
  --warm-interval-ms 1000 \
  --preflight-requests 3 \
  --ack-timeout-ms 300 \
  --timeout-ms 10000 \
  --quiet-perf-logs \
  --signing-ab-phases rsa,ecdsa,rsa \
  --results-dir results/yolo_2x2_cert_ab_same_topology_60s
```

This mode installs dual RSA/ECDSA certificates, keeps the same controller,
repo, topology, and NFD instances, and restarts only the compute providers for
each signing phase. The phase labels in `inference-latency-stats.json` are
`warm-rsa-1`, `warm-ecdsa-2`, and `warm-rsa-3`. Treat short runs as smoke tests
only; use the 60-second measured window above for evidence. A separate
`--single-rsa-certs` run is still useful as a compatibility smoke, but it is
not a fair signing-performance comparison if run under a different MiniNDN
RTT baseline.

`splitSigning` is certificate selection, not a SelectionMessage mode. RSA stays
the encryption certificate for NAC-ABE/permission unwrap. When an EC certificate
is available and `NDNSF_DISABLE_SPLIT_SIGNING` is not set, EC is used as the
signing certificate. Check the `NDNSF_CERT_SELECTION` lines in the user/provider
logs before interpreting a signing benchmark. `splitSigning=true` means
RSA-encryption plus EC-signing; `splitSigning=false` means the same RSA
certificate is used for both encryption and signing. Do not call this "split
selection"; selection-message behavior is controlled separately by normal or
compact selection.

For signing A/B/A runs, latency samples are the primary signal. The
`nfd-data-stats.json` and `traffic-stats.json` phase deltas cover the whole
phase window, including `--preflight-requests` and any first-use certificate,
repo, or control fetches. Short phases can therefore show extra Data packets
from the repo/certificate-publisher node even when the measured inference
latency is unchanged. Use `*PerObservedRequest` for a phase-average transport
view, and use `*PerMeasuredInference` only when `preflightRequestCount` is zero
or when deliberately amortizing preflight/control traffic over the measured
requests.

Use full packet tracing only as a last-resort external diagnosis for unexpected
or unexplained NDN names, stale Sync traffic, or startup/repo/keychain behavior.
Do not use a packet-trace run as a latency benchmark; tcpdump, pcap decoding,
and summary generation perturb the MiniNDN run. If packet tracing is necessary:

```bash
sudo -E env NDNSF_TIMELINE_TRACE_SAMPLE_RATE=10 \
  python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py \
  --layout 2x2 \
  --parallel-detect-scale-shards \
  --native-providers \
  --cold-requests 1 \
  --warm-requests 1 \
  --warm-duration-s 60 \
  --warm-interval-ms 1000 \
  --preflight-requests 3 \
  --ack-timeout-ms 300 \
  --timeout-ms 10000 \
  --quiet-perf-logs \
  --control-timing \
  --ndn-packet-trace \
  --ndn-packet-trace-nodes memphis,ucla,arizona,wustl,neu \
  --results-dir results/yolo_2x2_control_packet_trace_60s
```

`--ndn-packet-trace` starts full-snaplen `tcpdump` on the selected MiniNDN
nodes and decodes the pcaps with `ndndump` after the run. The script writes
`ndn-packet-trace-summary.json`, including each observed NDNSF/SVS name, the
per-node first/last observation timestamp, and the observed IP endpoint
directions. This is an external packet-level diagnostic. It can show when a
node's NFD-facing capture saw a Sync Interest or Data name, but it cannot decode
the NDNSF Request/ACK/Selection/Response semantic message carried inside an SVS
payload. Use it together with `svs-control-propagation-stats.json` and provider
lifecycle logs. The script now defaults `--ndn-packet-trace-window` to `warm` so
the pcap stays focused on the measured inference window; use `all` only when
debugging keychain, artifact deployment, repo, or startup synchronization.

For the older 2x3 Python-provider parallel-detect-scale baseline, use a
60-second warm window and explicit Python worker knobs:

```bash
PYTHONPATH=NDNSF-DistributedInference:$PYTHONPATH \
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py \
  --case yolo-layout \
  --layout 2x3 \
  --parallel-detect-scale-shards \
  --cold-requests 1 \
  --warm-requests 1 \
  --warm-duration-s 60 \
  --warm-interval-ms 1000 \
  --ack-timeout-ms 1500 \
  --timeout-ms 60000 \
  --provider-handler-workers 4 \
  --user-async-workers 4
```

A representative native-provider MiniNDN diagnostic on the current AI_Lab
topology is preserved at
`results/yolo_2x2_ailab_dependency_control_60s_latest`. It used the generated
native execution plan, C++ native provider executable, local ONNX Runtime role
runners, deterministic activation names, direct Selection prefetch, and
dependency timing. This result is for the small `yolo26n.pt` workload at
input size 32; use it as the canonical tiny-model DI baseline. The 60 measured
warm requests produced warm p50 53.75 ms, p95 79.49 ms, and max 93.49 ms.
The same run recorded about 23.98 NFD Data
packets and about 143.81 KB of NFD `nOutBytes` per measured warm inference.
`nfd-data-stats.json` reports `data_packets_per_inference` and
`avg_data_packet_bytes`; the byte average is an approximate transport-size
ratio because NFD byte counters include all packet types on the measured faces.
`traffic-stats.json` also reports total node bytes per inference.

The same run confirmed planned-name prefetching in the native provider path:
256/256 dependency fetches used deterministic planned names, ref-wait p50 was
0.00 ms, and provider input-fetch-wait p50 was about 13.69 ms. ONNX was not the
p50 bottleneck for this tiny model: ONNX run sum p50 was about 2.37 ms across
the role chain, while dependency fetch max p50 was about 28.41 ms and outer
control residual p50 was about 24.94 ms. Compared with the older AI_testbed
diagnostic, AI_Lab reduced MiniNDN RTT noise and outer-control residual, but
activation delivery still costs tens of milliseconds. The remaining latency is
therefore dominated by NDNSF outer Request/ACK/Selection/Response propagation
and activation dependency fetch waits, not by local ONNX execution.

When comparing later benchmark runs, first check the printed
`YOLO_LAYOUT_RUN_CONFIG` line. In particular, `yolo26n.pt --input-size 32` and
`yolov8n.pt --input-size 32` are different workloads. A current 60-second
low-overhead `yolo26n.pt` run in
`results/yolo26n_32_2x2_current_default_60s_latest` produced warm p50
63.57 ms and p95 80.54 ms while MiniNDN edge RTT samples were higher than the
canonical run. A 60-second narrow timing run in
`results/yolo26n_32_2x2_current_timing_60s_latest` produced p50 62.14 ms and
showed the same structure as the canonical result: provider dataflow p50 about
30.00 ms, dependency-fetch-max p50 about 28.82 ms, outer-control residual p50
about 33.34 ms, and ONNX run sum p50 about 2.99 ms. By contrast,
`yolov8n.pt --input-size 32` in
`results/yolov8n_32_2x2_current_default_60s_latest` produced output shape
`(1, 84, 21)` and warm p50 85.98 ms. Do not treat that as a regression from
the `yolo26n.pt` tiny-model baseline; it is a heavier model/output shape and
must be tracked as a separate benchmark line.

For outer-control analysis, use a narrow `--control-timing` run rather than
packet tracing. `results/yolo26n_32_2x2_current_control_60s_latest` produced
warm p50 58.10 ms and p95 84.53 ms. In that run, SVS propagation was not the
p50 bottleneck by itself: Request SVS to provider request receive was about
3.33 ms p50, ACK SVS to user pre-decrypt about 3.23 ms p50, Selection SVS to
provider selection receive about 3.26 ms p50, and Response SVS to user observe
about 3.28 ms p50. The user saw first ACK about 7.06 ms after request publish,
ACK matching took about 0.71 ms p50, and selected providers observed direct
Selection prefetch Data about 3.02 ms p50 after the user put the selection
Data. The larger `Selection -> Response` interval mostly contained final
provider execution/dataflow: final-provider execution-start-to-done was about
30.77 ms p50, while selection-received-to-execution-start and response decrypt
to callback were both sub-millisecond at p50. This evidence says the next
optimization target should be final Merge/dataflow and activation fetch
latency, not further SelectionMessage size compression.

An edge-RTT diagnostic run is preserved at
`results/yolo_2x2_ailab_edge_rtt_60s_latest`. It uses the same AI_Lab topology
but also starts short ndnping probes along the generated dependency edges, so it
should be treated as a latency-cause diagnostic rather than a replacement for
the canonical baseline above. It produced warm p50 63.86 ms and p95 85.64 ms.
More importantly, it showed dependency fetch p50 20.90 ms, dependency fetch max
p50 29.53 ms, and provider dataflow p50 31.00 ms. The edge RTT p50 samples were
about 16.75 ms for Backbone -> Head/Shard/0, 15.44 ms for Backbone ->
Head/Shard/1, 20.35 ms for Head/Shard/0 -> Merge, and 5.60 ms for
Head/Shard/1 -> Merge. This confirms that activation latency should be
interpreted with provider-to-provider edge RTT, not only the user-to-provider
baseline.

The more detailed outer-control breakdown run
`results/yolo_2x2_ailab_outer_breakdown_60s_latest` keeps the same 60-second
AI_Lab recipe and adds a split for Selection -> final provider -> Response ->
User. It produced warm p50 63.52 ms and p95 87.11 ms. Warm SVS propagation p50
was about 3.99 ms for Request, 3.89 ms for ACK, 4.41 ms for Selection, and
3.95 ms for Response. The final-provider breakdown showed Selection publication
to final-provider selection receive p50 5.05 ms, final-provider response publish
to user observe p50 4.06 ms, and final-provider selection receive to response
publish p50 about 33.22 ms. Therefore, in this diagnostic, the long
`selection_to_response` component is not mostly Selection or Response delivery;
it is the final Merge provider's dataflow/handler path, which is dominated by
dependency fetch waits from the head shards.

The same script also enables `NDNSF_COLLAB_LARGE_FETCH_TIMING=1` and writes
`collab-large-fetch-stats.json`. That file records Core-level SegmentFetcher
elapsed time, encoded object size, and InterestLifetime for each collaboration
large-data fetch. Use it together with `dependency-input-timing-stats.json` to
separate native segmented fetch cost from provider scheduling and tensor decode
cost. It also enables `NDNSF_PENDING_IMS_TIMING=1` and writes
`pending-ims-timing-stats.json`, which records whether predictable activation
Interests reached the producer before the Data was inserted into in-memory
storage. In the representative native-provider run above, Core-level
collaboration fetches completed 256/256 times with no errors, elapsed p50
16.70 ms, elapsed p95 33.75 ms, first-segment p50 16.62 ms, encoded object p50
3839 bytes, received/validated segment p50 1, and InterestLifetime p50
10000 ms. `activation-segment-timeline-stats.json` showed segment
interest-to-Data p50 18.46 ms; this split into interest-to-active-put p50
10.30 ms, active-put-to-Data p50 8.90 ms, and Data-to-validated p50 0.07 ms.
`pending-ims-timing-stats.json` showed 245 pending activation Interests that
were later satisfied, with pending-age p50 8.03 ms. The same run also wrote
`dependency-frontier-timing-stats.json`: 256 output/fetch pairs joined by
deterministic Data name, producer-output-ready to consumer-first-segment p50
9.00 ms, publish-done to consumer-first-segment p50 9.00 ms, and
producer-output-ready to fetch-complete p50 9.00 ms. That confirms planned
prefetch is reaching the producer before the corresponding activation Data
exists. The remaining cost is therefore mostly outer control propagation,
stage-frontier scheduling, activation delivery after active-put, and final
result delivery rather than ONNX execution, tensor decode, segment validation,
or segment window size.

When comparing cold and warm inference, keep the user process model in mind.
If a script launches cold and warm as separate user processes, in-memory plan
cache state and recent-responder history cannot carry across the boundary.
Stable P95 measurements should use multiple sequential requests in the same
user process, or the 60-second warm window supported by the MiniNDN runner.

The MiniNDN script clears the provider artifact cache before the first command.
It starts a repo node on `neu`, starts the controller on `memphis`, and then runs
a controller-side deployer that writes the model shards and runner into the
repo. Python-provider runs show `NDNSF_EXECUTION_ARTIFACT_CACHE_MISS ... source=repo`
and later `NDNSF_EXECUTION_ARTIFACT_CACHE_HIT`. Native-provider runs show
`NDNSF_DI_NATIVE_PROVIDER_REPO_SEGMENT_FETCH` followed by
`NDNSF_DI_NATIVE_PROVIDER_ARTIFACTS_MATERIALIZED` and
`NDNSF_DI_NATIVE_PROVIDER_PROVISION_READY`, proving that the C++ provider
materialized repo-backed artifacts, then switched readiness from installing to
ready before it could be selected. The quick-suite `di-minindn-native` case is
intentionally cold-only so it gates artifact deployment and native dataflow
without mixing in the separate warm ACK/SVS stability benchmark.

The YOLO Python provider can also use the generic artifact provisioning
readiness helper with `--artifact-references` and `--artifact-cache-dir`.
When enabled, it materializes ONNX role artifacts into a provider-local cache
in the background and reports `runtimeStatus=installing` until the selected
roles are ready. The older request-time dynamic provisioning path remains as a
fallback for compatibility. The native C++ provider now follows the same
readiness contract in serving mode: the service is registered first, ACKs remain
negative while artifacts install asynchronously, and the real native
collaboration handler is attached only after runner construction succeeds.

To run the APP API smoke, the local ONNX executor smoke, and both stable
MiniNDN split smokes through one entry point:

```bash
sudo -E python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case all
sudo -E python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case yolo-layout --layout 2x3
python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case yolo-layout-local --layout 3x2
```

`yolo-layout` validates custom YOLO layout export, repo-backed artifact
deployment, dependency prefetch, activation exchange, and final result delivery
through MiniNDN. `yolo-layout-local` remains available when only the generated
chunk graph and local ONNX correctness need to be checked quickly.
The currently validated network-level custom layouts are `2x3` and `3x2`.
`1x3` and `3x3` are useful as fast local export smokes, while `3x3` should be
run through `yolo-layout` before it is treated as a deployment baseline.

The policy/repo inspection helper is still available:

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_2x2/plan_example.py
```

It builds four assignable roles:

```text
/Stage/0/Shard/0
/Stage/0/Shard/1
/Stage/1/Shard/0
/Stage/1/Shard/1
```

and three dependency scopes:

```text
stage0-internal   activation transfer inside stage 0
stage0-to-stage1  activation transfer between pipeline stages
stage1-internal   activation transfer inside stage 1
```

`split_model.py` writes per-role ONNX artifacts into the generated deployment
policy. Each role has its own ONNX chunk. This is an execution plan over real
YOLO layers, not a synthetic NumPy model, but the current YOLO chunks form a
pipeline-sequential dependency graph: each provider fetches the previous
chunk's activation reference, continues the ONNX computation, and publishes the
next activation reference. The final chunk publishes the response. The user
compares that response with a local full YOLO forward pass and prints `ok=true`
only when the values match.

A provider can advertise all four roles without understanding NDN internals:

```python
provider = APPProvider.from_config("yolo_policy.yaml", provider_id="A")
provider.serve_service(
    service="/AI/YOLO/2x2Inference",
    roles="all",
    handler=handle_yolo_role,
    backends=["onnxruntime"],
    temp_dir="/tmp/provider-A",
    has_model=False,
    can_provision=True,
    allow_executables=True,
)
provider.run()
```

Inside `handle_yolo_role(ctx)`, the APP uses normal Python model logic and the
provided collaboration context:

```python
if ctx.role == "/Stage/0/Shard/0":
    hidden = run_stage0_shard0(ctx.execution.path("model"), ctx.request)
    ctx.publish_output(hidden, key_scope="stage0-to-stage1",
                       topic_suffix="Stage-0-Shard-0")
```

For more complex layouts, the APP changes role names and dependency scopes; the
NDNSF-facing deployment, artifact, and security mechanics stay the same.

`Experiments/NDNSF_DI_Yolo2x2_Repo_Minindn.py` is still useful, but it is a
DistributedRepo storage smoke test. Use `Experiments/NDNSF_DI_Yolo2x2_Minindn.py`
when you want to verify end-to-end split inference and result consistency.
