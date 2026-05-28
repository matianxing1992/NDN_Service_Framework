# NDNSF-DistributedInference

NDNSF-DistributedInference is a Python library built on top of the generic
NDNSF Python API. It provides higher-level APIs for distributed AI inference
without adding AI-specific semantics to NDNSF Core.

Layering:

```text
APP
  calls model/inference APIs

NDNSF-DistributedInference
  understands model plans, roles, stages, shards, runtime artifacts,
  backend requirements, and inference dependencies

NDNSF Python Wrapper
  exposes generic service invocation, collaboration, encrypted artifacts,
  segmented large data, and provider callbacks

NDNSF Core
  handles Face, SVS, NAC-ABE, signing, permissions, selection, workers,
  and wire protocol behavior
```

## Application-Level API

The recommended API for application developers is `APPClient`,
`APPProvider`, `APPController`, `APPDeployment`, and `InferencePlanBuilder`.
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

Client side:

```python
from ndnsf_distributed_inference import APPClient
from ndnsf_distributed_inference.backends.onnxruntime import runtime_spec

client = APPClient.from_config("yolo_policy.yaml")
builder = client.plan_builder(
    "/AI/YOLO/SplitInference",
    runtime=runtime_spec(),
)
builder.add_part(
    role="/Stage/0",
    model="yolo-stage0.onnx",
    kind="onnx-model",
).add_part(
    role="/Stage/1",
    model="yolo-stage1.onnx",
    kind="onnx-model",
)

result = client.infer(builder.build(), input_payload)

# Multiple requests can be submitted concurrently. Each request still uses the
# NDNSF runtime for Face/SVS/NAC-ABE work; the APP thread receives a Future.
future = client.infer_async(builder.build(), input_payload)
result = future.result(timeout=30)
```

Provider side:

```python
from ndnsf_distributed_inference import APPProvider

provider = APPProvider.from_config("yolo_policy.yaml", provider_id="A")
provider.serve(
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

For multi-service deployments, call `provider.serve(...)` once per service and
use `client.infer_async(...)` for concurrent requests across one or more
services. The service name still determines the fixed role set and dependency
graph used for each request.

## Example Families

The repository currently includes three Python example families under
`examples/python/NDNSF-DistributedInference/`:

```text
yolo_split/
  Two-stage YOLO-style split inference over ONNX Runtime.

yolo_2x2/
  Plan-generation smoke test for a two-stage, two-shard layout.

pytorch_eager_2x2/
  Four-provider PyTorch eager example. It splits a random fully connected
  network into two stages and two shards per stage, then verifies the
  distributed result against a local full-model reference.
```

The ONNX examples represent the preferred portable deployment path when a
model can be exported to ONNX. The PyTorch eager example represents the most
flexible Python runtime path for dynamic control flow, custom Python logic, and
research prototypes.

The lower-level `DistributedInferenceClient`,
`DistributedInferenceProvider`, and `DistributedInferenceController` remain
available for framework developers and experiments that need direct control.

## Lower-Level API Sketch

User side:

```python
from ndnsf_distributed_inference import (
    DistributedInferenceClient,
    load_or_generate_deployment,
)

deployment = load_or_generate_deployment("yolo_policy.yaml", "/tmp/yolo-policy")
client = DistributedInferenceClient.connect(
    group=deployment.group,
    controller=deployment.controller,
    user=deployment.user,
    trust_schema=deployment.trust_schema,
)
result = client.infer(plan, input_payload)
```

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
does not infer model dependencies. It carries the dependency graph recorded in
the service config and converts the plan into generic NDNSF collaboration calls
and artifact provisioning.

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
    controller="/example/hello/controller",
    group="/example/hello/group",
    user="/example/hello/user",
    provider_prefix="/example/hello/provider",
    trust_app_roots=["/example"],
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

The generated YAML is therefore deployment policy derived from the split,
not handwritten dependency logic. The same service name should always map to
the same model layout and dependency graph. If a model is split differently,
publish it as a different service name.

Provider handlers receive a role-local dependency view as `ctx.dependencies`,
so handler code can ask what the current role should publish or wait for
instead of repeating topic strings by hand:

```python
def handle_assigned_role(ctx):
    if ctx.dependencies.outputs:
        activation = run_local_stage(ctx.execution.path("model"), ctx.request)
        large_name = ctx.publish_output_large(activation)
        edge = ctx.dependencies.output()
        ctx.ndnsf.publish(edge.key_scope, edge.topic("ref"), large_name.encode())

    if ctx.dependencies.inputs:
        edge = ctx.dependencies.input()
        ref = ctx.ndnsf.wait_one(edge.key_scope, edge.topic("ref"), 10000)
```

For roles with multiple inputs or outputs, pass `key_scope` explicitly, e.g.
`ctx.dependencies.input("stage0-to-stage1")` or
`ctx.publish_output(payload, key_scope="stage1-internal")`.

## User-Facing Security Config

Application developers do not need to hand-write NDN validator trust schema.
They describe the deployment in YAML or JSON:

```yaml
application: yolo-split-demo
controller: /example/hello/controller
group: /example/hello/group

identities:
  user: /example/hello/user
  provider_prefix: /example/hello/provider

trust:
  app_roots: [/example]
  # Production deployments should use an explicit trust anchor.
  # anchor_file: /path/to/root.cert

artifact_security:
  # Executable artifacts are rejected unless all three are configured:
  # trust.anchor_file, artifact_security.allowlist, and sandbox.
  allowlist: []
  sandbox:
    kind: ""

services:
  - name: /AI/YOLO/SplitInference
    model: /Model/Ultralytics/YOLO/Split
    roles: [/Stage/0, /Stage/1]
    dependencies:
      - producers: [/Stage/0]
        consumers: [/Stage/1]
        key_scope: stage0-to-stage1
        topic_prefix: /activation
    users: [/example/hello/user]
    providers:
      - identity: /example/hello/provider
        roles: all
      - identity: /example/hello/provider/A
        roles: all
```

The distributed-inference layer compiles this config into an NDNSF controller
policy and an NDN trust schema:

```bash
ndnsf-di-policy \
  --config examples/python/NDNSF-DistributedInference/yolo_split/yolo_policy.yaml \
  --out-dir /tmp/ndnsf-di-yolo-policy
```

Generated files:

```text
/tmp/ndnsf-di-yolo-policy/trust-schema.conf
/tmp/ndnsf-di-yolo-policy/controller.policies
/tmp/ndnsf-di-yolo-policy/service-manifest.json
/tmp/ndnsf-di-yolo-policy/service-manifest.json.sha256
```

The service manifest is a canonical JSON form of the service-to-model and
service-to-dependency mapping. The `.sha256` file is only a local fingerprint
for deployment tooling; it is not a security signature. Security comes from
publishing the manifest as NDN Data and validating the Data signature.

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
user publishes encrypted model/runtime artifacts and requests two providers to
cooperate. Providers download their assigned ONNX shard and a generic ONNX
Runtime runner into provider-local temporary directories.

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
  --out-dir /tmp/ndnsf-yolo-split \
  --policy /tmp/ndnsf-yolo-split/yolo_policy.yaml
```

After installation, application code can import the distributed inference layer
from any working directory:

```python
from ndnsf_distributed_inference import DistributedInferenceClient
```

Run one role per shell or MiniNDN node:

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_split/controller.py
python3 examples/python/NDNSF-DistributedInference/yolo_split/provider.py \
  --temp-dir /tmp/ndnsf-yolo-stage0
python3 examples/python/NDNSF-DistributedInference/yolo_split/provider.py \
  --provider-id A --temp-dir /tmp/ndnsf-yolo-stage1
python3 examples/python/NDNSF-DistributedInference/yolo_split/user.py
```

## YOLO 2x2 Split API Example

The `yolo_2x2` example shows how the same APP API expresses a more general
layout with two pipeline stages and two tensor-parallel shards per stage:

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
stage0-internal   stage-0 shard exchange
stage1-internal   stage-1 shard exchange
stage0-to-stage1  activation transfer between pipeline stages
```

The example uses placeholder ONNX shard payloads so it is fast enough for API
testing. A real APP supplies actual exported model shards and writes the role
handler logic that publishes and waits for tensors according to the model's
dependency pattern. The framework does not assume that models are YOLO-specific
or limited to 2x2; roles and dependencies are arbitrary NDN-style names.

A provider can advertise all four roles without understanding NDN internals:

```python
provider = APPProvider.from_config("yolo_policy.yaml", provider_id="A")
provider.serve(
    service="/AI/YOLO/2x2Inference",
    roles="all",
    handler=handle_yolo_role,
    backends=["onnxruntime"],
    temp_dir="/tmp/provider-A",
)
provider.run()
```

Inside `handle_yolo_role(ctx)`, the APP uses normal Python model logic and the
provided collaboration context:

```python
if ctx.role == "/Stage/0/Shard/0":
    partial = run_stage0_shard0(ctx.execution.path("model"), ctx.request)
    ctx.publish_internal(partial, key_scope="stage0-internal",
                         topic_suffix="/tensor/shard0")
    peer = ctx.wait_internal(key_scope="stage0-internal",
                             topic_suffix="/tensor/shard1",
                             timeout_ms=5000)
```

For more complex layouts, the APP changes role names and dependency scopes; the
NDNSF-facing deployment, artifact, and security mechanics stay the same.

The generic ONNX Runtime runner is identified as:

```text
/Runtime/ONNXRuntime/CPU
```

It is still delivered as an encrypted artifact when the selected provider does
not already have it. The framework verifies the artifact hash and materializes
it locally, but it does not execute downloaded code automatically. In this
example the runner is treated as a non-executable Python script and launched
explicitly through the provider's local Python interpreter. Executable
artifacts are disabled by default; applications must opt in explicitly before
the framework marks an artifact executable.
