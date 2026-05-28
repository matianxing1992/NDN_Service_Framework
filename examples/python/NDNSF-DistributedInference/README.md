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
produces the service config, dependency graph, and artifact references; the
controller, providers, and user then load the generated config.

`yolo_2x2/` is a lightweight plan-generation smoke test for a two-stage,
two-shard service layout.

`pytorch_eager_2x2/` shows a four-provider Python runtime deployment. It splits
a small fully connected network into two stages and two shards per stage, then
checks the distributed output against the local full model.

## MiniNDN Smoke

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
