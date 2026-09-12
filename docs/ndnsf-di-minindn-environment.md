# NDNSF-DI MiniNDN environment profile

`Experiments/NDNSF_DI_LlmPipeline_Minindn.py` is the maintained protocol
runner.  Local smoke tests and a run on another machine use the same runner
and request path; machine-specific inputs are supplied by a small JSON profile.
The checked-in example is
`Experiments/profiles/ndnsf-di-minindn-local.example.json`.

Run locally with:

```bash
python3 Experiments/NDNSF_DI_LlmPipeline_Minindn.py \
  --environment-profile Experiments/profiles/ndnsf-di-minindn-local.example.json \
  --runtime qwen-onnx-cpu-native \
  --native-provider-binary build-spec184-b5-candidate-r4/examples/di-native-provider
```

On the target machine, copy the example to an untracked profile and change
only the topology, stage node names, model/content paths, temporary roots and
native executable paths.  Workload arguments (`--runtime`, request identity,
prompt, timeouts, Selection mode and model manifest) stay in the command or
the reviewed caller.  Explicit command-line values override profile fields.

The profile schema is `ndnsf-di-minindn-environment-v1`.  Relative paths are
anchored at the repository root; absolute paths are accepted for machine
storage.  `stageNodes` must contain two to four unique MiniNDN node names.
The runner checks the topology and native executable before creating the
MiniNDN namespace, then writes `environment-profile-resolved.json` under the
run output with the profile digest and resolved non-secret values.

This profile does not make a MiniNDN run multi-host or qualify a model.  A
remote/Tiger run still needs its sealed SIF and external model identity, and
those remain separate Spec184 evidence gates.
