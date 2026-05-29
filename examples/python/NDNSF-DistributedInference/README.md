# NDNSF-DistributedInference Examples

These examples use the high-level `ndnsf_distributed_inference` package. App
code should import this package rather than using the lower-level `ndnsf`
wrapper directly.

Install the Python packages in editable mode from the repository root:

```bash
python3 -m pip install -e ./pythonWrapper
python3 -m pip install -e ./NDNSF-DistributedInference
```

## Examples

`yolo_split/` shows a two-stage ONNX Runtime deployment. The model splitter
produces the service config, input/output contract, dependency graph, and
artifact references; the controller, providers, and user then load the
generated config. Run `split_model.py` first so the generated policy records
the ONNX shard paths before providers start.

`yolo_2x2/` shows a four-provider 2x2 split-inference deployment. It uses a
deterministic YOLO-style tensor pipeline, splits it into two stages and two
shards per stage, and checks the distributed result against the local full
model. Its MiniNDN test runs a separate repo node. The controller-side deployer
stores model shards and the executable runner in that repo before inference;
the user only assigns roles with repo manifest references. Providers start
without local model/runtime files, fetch the required artifacts on the first
command, and reuse the provider artifact cache on the second command.
`plan_example.py` remains a policy/repo inspection helper.
Repo artifact and intermediate Data names are publisher-scoped: controller
artifacts are under the controller namespace, user inputs/intermediates are
under the user namespace, and provider outputs are under provider namespaces.
Generated trust schemas validate data and certificates hierarchically; a
production deployment should use a trust-root certificate as the anchor.

`pytorch_eager_2x2/` shows a four-provider Python runtime deployment. It splits
a small fully connected network into two stages and two shards per stage, then
checks the distributed output against the local full model. Its policy records
the tensor payload contract and per-role PyTorch state artifact paths.

## MiniNDN Smoke

Run the YOLO 2x2 MiniNDN split-inference smoke test from the repository root:

```bash
sudo -n python3 Experiments/NDNSF_DI_Yolo2x2_Minindn.py
```

Expected output includes:

```text
YOLO_2X2_RESULT ... ok=true
YOLO_2X2_DYNAMIC_PROVISIONING_MININDN_OK ...
```

Provider logs should contain `NDNSF_EXECUTION_ARTIFACT_CACHE_MISS` for the
first command and `NDNSF_EXECUTION_ARTIFACT_CACHE_HIT` for the second command.

Run the PyTorch eager 2x2 MiniNDN smoke test from the repository root:

```bash
sudo -n python3 Experiments/NDNSF_DI_PyTorch2x2_Minindn.py
```

Expected output includes:

```text
PYTORCH_2X2_RESULT ... ok=true
PYTORCH_2X2_MININDN_OK ...
```

If the user process prints `PYTORCH_2X2_MININDN_TEARDOWN_WARNING`, the
inference completed correctly but the native Python wrapper still has a cleanup
path that should be improved. Treat that as a lifecycle warning, not an
inference correctness failure.
