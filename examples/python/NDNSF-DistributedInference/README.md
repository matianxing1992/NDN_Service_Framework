# NDNSF-DistributedInference Examples

This directory contains runnable examples for the high-level
`ndnsf_distributed_inference` APP API. If you are new to this API, start here
instead of calling the lower-level `ndnsf` Python wrapper directly.

The examples demonstrate the same workflow:

```text
1. Generate or review a policy YAML.
2. Start an APPController from that policy.
3. Start providers from that policy.
4. Start a user/client from that policy.
5. The user calls distributed_inference(service, input_object).
6. Providers exchange activation objects according to the dependency graph.
7. The user receives final predictions.
```

The important point is that the user does not manually choose NDN names,
Interest/Data packets, segment names, NAC-ABE attributes, or provider-specific
dependency topics. The service policy and APP API hide that machinery.

## Install

From the repository root:

```bash
python3 -m pip install -e ./pythonWrapper
python3 -m pip install -e ./NDNSF-DistributedInference
```

For ONNX examples, install the model/runtime packages needed by the example
environment, such as `numpy`, `onnx`, and `onnxruntime`. The MiniNDN scripts
also assume the repository has been built and that the native NDNSF runtime can
be found by the Python wrapper.

## What Each Example Shows

```text
yolo_split/
  Two-stage ONNX Runtime inference.
  One provider runs Stage 0, another provider runs Stage 1.
  Stage 0 publishes an activation object; Stage 1 fetches it and finishes.

yolo_2x2/
  Four-role ONNX Runtime inference.
  Stage 0 has two sequential shards and Stage 1 has two sequential shards.
  The example uses repo-backed dynamic provisioning: providers can start
  without local model/runtime files, then fetch the assigned role artifact.

pytorch_eager_2x2/
  Four-role fully connected ONNX inference generated from a PyTorch-defined
  model. The splitter exports a full ONNX reference graph, analyzes candidate
  cut points, and writes four ONNX shards: two hidden-layer shards and two
  output-layer shards. The distributed output is checked against the local
  full-model reference.
```

For a deployment-oriented ONNX path, use `yolo_split/` or `yolo_2x2/`.
For a compact model-specific splitter that demonstrates horizontal splitting
inside fully connected layers, use `pytorch_eager_2x2/`.

## Policy Files

Each example has a `*_policy.yaml`. This file is the deployment contract:

```text
runtime.user_identity       identity used by the client/user process
services[].users            exact users allowed to invoke each service
services[].providers        exact providers allowed to provide each service
services[].roles            model stages/shards that can be assigned
services[].dependencies     tensor/object edges between roles
services[].artifacts        model/runtime files or repo-backed artifacts
services[].input/output     request and response payload descriptions
```

Generated policies are split into:

```text
# editable deployment section
  Names, controller/group prefixes, runtime user, trust root, artifact policy.

# generated authorization summary
  Read-only summary of which users/providers can use/provide which services.

# generated model-plan section
  Services, roles, dependencies, artifacts, input/output contracts.
```

Real authorization comes from `services[].users` and `services[].providers`.
The summary is only a review aid.

Before running an example, inspect the policy:

```bash
PYTHONPATH="NDNSF-DistributedInference:$PYTHONPATH" \
python3 -m ndnsf_distributed_inference.policy \
  --config examples/python/NDNSF-DistributedInference/yolo_2x2/yolo_policy.yaml \
  --out-dir /tmp/ndnsf-di-policy-review \
  --print-summary
```

This prints user permissions, provider permissions, role coverage, artifact
coverage, and artifact-security status. `--explain` is an alias for
`--print-summary`.

## Optional GUI

If you want to generate or inspect a policy without editing YAML by hand, start
the lightweight DI GUI from the repository root:

```bash
PYTHONPATH="NDNSF-DistributedInference:$PYTHONPATH" \
python3 Experiments/NDNSF_DI_GUI.py
```

For a one-command GUI preflight and optional MiniNDN smoke:

```bash
python3 Experiments/NDNSF_DI_GUI_Minindn.py
python3 Experiments/NDNSF_DI_GUI_Minindn.py --run-minindn --case app-api --no-gui
python3 Experiments/NDNSF_DI_GUI_Minindn.py --run-minindn --case yolo-2x2 --no-gui
```

The GUI has tabs for creating a service project, editing and validating a
policy, inspecting ONNX graph split candidates, selecting local `ndnsec`
identities, and launching example controller/provider/user processes. Its
Deployment Runner defaults to the YOLO 2x2 MiniNDN smoke path, which starts
MiniNDN, executes distributed inference, and reports
`YOLO_2X2_RESULT ... ok=true`. The auto-split two-stage path is also available
as a selectable regression case. The `Controller`, `User`, and `Provider` tabs
are separate because a real node may run one or more roles at the same time. It
uses the same policy format and validation logic as the command-line tools.
The role tabs also include the normal NDN certificate workflow: users and
providers generate their own key requests, the controller/root node signs those
requests, and the signed certificates are copied back and installed on the
requesting nodes.

## User-Side API

Application users should normally write only this:

```python
from ndnsf_distributed_inference import APPClient

client = APPClient.from_config("yolo_policy.yaml")
service = "/AI/YOLO/SplitInference"

print(client.describe_input(service))
print(client.describe_output(service))

result = client.distributed_inference(service, image_tensor)
```

For concurrent requests:

```python
future = client.async_distributed_inference(service, image_tensor)
result = future.result(timeout=30)
```

If the policy declares `codec: npz`, common numpy tensor inputs are encoded
automatically. Use a custom input encoder only for model-specific preprocessing
that a generic tensor encoder cannot know.

## Provider-Side API

Providers register the service and advertise roles:

```python
from ndnsf_distributed_inference import APPProvider

provider = APPProvider.from_config("yolo_policy.yaml", provider_id="A")
provider.serve_service(
    service="/AI/YOLO/SplitInference",
    roles="all",
    handler=handle_assigned_role,
    backends=["onnxruntime"],
    temp_dir="/tmp/ndnsf-di-provider-A",
    has_model=False,
    can_provision=True,
)
provider.run()
```

In the homogeneous-worker case, all providers can advertise `roles="all"`.
The assignment tells each selected provider which role to run. If dynamic
provisioning is enabled, the provider fetches the model/runtime artifact for
that assigned role.

## Controller API

The controller starts from the same policy:

```python
from ndnsf_distributed_inference import APPController

controller = APPController.from_config("yolo_policy.yaml")
controller.run()
```

The controller distributes NDNSF permissions and trust material generated from
the policy. In real deployments, run it on the node that owns the controller
identity and trust root.

## Running the MiniNDN Examples

Run these commands from the repository root.

Two-stage YOLO ONNX split:

```bash
sudo -E python3 Experiments/NDNSF_DI_YoloSplit_Minindn.py
```

Expected markers:

```text
YOLO_SPLIT_RESULT ... ok=true
YOLO_SPLIT_MININDN_OK ...
```

Four-role YOLO 2x2 ONNX split:

```bash
sudo -E python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py
```

Expected markers:

```text
YOLO_2X2_RESULT ... ok=true
YOLO_2X2_DYNAMIC_PROVISIONING_MININDN_OK ...
```

Four-role PyTorch-defined fully connected ONNX split:

```bash
sudo -E python3 Experiments/NDNSF_DI_PyTorch2x2_Minindn.py
```

Expected markers:

```text
PYTORCH_2X2_RESULT ... ok=true
PYTORCH_2X2_MININDN_OK ...
```

The unified runner can launch all supported DI cases:

```bash
sudo -E python3 Experiments/NDNSF_DI_Run_Minindn_Regressions.py --case all
```

## Manual Multi-Process Run

MiniNDN is the easiest way to run a full topology, but you can also run the
same APP roles manually in separate shells if local routing and certificates
are already prepared.

For `yolo_split/`:

```bash
python3 examples/python/NDNSF-DistributedInference/yolo_split/split_model.py

python3 examples/python/NDNSF-DistributedInference/yolo_split/controller.py \
  --config examples/python/NDNSF-DistributedInference/yolo_split/yolo_policy.yaml

python3 examples/python/NDNSF-DistributedInference/yolo_split/provider.py \
  --config examples/python/NDNSF-DistributedInference/yolo_split/yolo_policy.yaml \
  --provider-id A \
  --temp-dir /tmp/ndnsf-yolo-stage0

python3 examples/python/NDNSF-DistributedInference/yolo_split/provider.py \
  --config examples/python/NDNSF-DistributedInference/yolo_split/yolo_policy.yaml \
  --provider-id B \
  --temp-dir /tmp/ndnsf-yolo-stage1

python3 examples/python/NDNSF-DistributedInference/yolo_split/user.py \
  --config examples/python/NDNSF-DistributedInference/yolo_split/yolo_policy.yaml
```

Manual runs are useful for debugging APIs, but MiniNDN is preferred for
network-level regression because it starts per-node NFDs and routes.

## Dynamic Provisioning and Repo-Backed Artifacts

`yolo_2x2/` demonstrates dynamic provisioning. The controller-side deployer
stores model shards and runtime bundles in an NDNSF-DistributedRepo node before
inference. The generated repo deployment manifest carries both `repoManifest`
and `largeDataReference` metadata for each artifact instead of large model
bytes. Selected providers fetch their assigned role artifacts, cache them
locally, execute the role, and publish activation objects for dependent roles.

Provider logs should show:

```text
NDNSF_EXECUTION_ARTIFACT_CACHE_MISS
NDNSF_EXECUTION_ARTIFACT_CACHE_HIT
```

The first command should miss and fetch artifacts. Later commands can reuse
the provider artifact cache.

## Common Mistakes

Use `--print-summary` first if anything looks odd. Frequent mistakes are:

```text
runtime.user_identity is not listed in any service users list
provider identity is not listed in service providers
service role has no authorized provider
artifact path points to a missing file
repo manifest was not published before inference
input tensor shape/dtype does not match client.describe_input(service)
provider has can_provision=False but does not have local model shards
production trust.anchor_file is missing
```

If a MiniNDN run prints a teardown warning after an `ok=true` result, treat it
as a lifecycle cleanup warning rather than inference failure. The correctness
markers are the `... RESULT ... ok=true` lines.
