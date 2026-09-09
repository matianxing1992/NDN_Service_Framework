# R10-B72 Provider Native Plan Check

## Scope

本批次对当前 `di-native-provider` executable 使用已有四角色 native bundle 做
metadata/plan `--check-only` 预检。检查不创建服务 registration、不发送 NDN 请求，也不
把 plan readiness 当作 Provider transport 或 T016 qualification。

## First boundary and corrected run

首轮命令省略 `--service`，CLI 使用默认 `/AI/YOLO/2x2Inference`，而 bundle plan 的服务
是 `/Inference/NativeTracer`，因此在 plan 解析前返回：

```text
error: native execution plan has no service: /AI/YOLO/2x2Inference
rc=2
```

原始输出保留在 `.codex-tmp/spec182-r10-b72-provider-check-20260909/check-only.log`。
修正命令显式绑定 bundle service 后通过：

```text
env LD_LIBRARY_PATH=.codex-tmp/spec182-r4-b2/build:\
/home/tianxing/NDN/nac-abe-integration-182/install/lib:\
/home/tianxing/NDN/ndn-svs/build \
  .codex-tmp/spec182-r4-b2/build/examples/di-native-provider \
  --plan native-execution-plan.json --manifest service-manifest.json \
  --service /Inference/NativeTracer --check-only --trust-schema trust-schema.conf
  rc=0
```

Observed markers:

```text
NDNSF_DI_NATIVE_PROVIDER_START mode=check service=/Inference/NativeTracer
NDNSF_DI_NATIVE_PROVIDER_BACKENDS_READY onnxruntime=1
NDNSF_DI_NATIVE_PROVIDER_PLAN_READY roles=4 artifacts=4 activeRoles=4 runners=4
NDNSF_DI_EXECUTION_EVIDENCE ... realCompute="true" runtimeVersion="1.26.0"
NDNSF_DI_NATIVE_PROVIDER_CHECK_OK service=/Inference/NativeTracer roles=4 artifacts=4 registered=4 workers=1
```

## Review and boundary

Static review covered service-name selection, metadata-only plan parsing, role/artifact counts,
execution-evidence identity, loader paths and the no-network `check-only` branch. No product
source change was needed. Candidate library resolution used the current DI/framework build,
the explicit NAC-ABE integration prefix and ndn-svs build; no `not found` dependency was
observed in the prior target closure check.

The corrected run closes only local Provider plan/manifest readiness. Provider `--serve` with
Controller permission, authenticated Selection, post-Selection worker assembly, independent
requester/Provider transport, conversation, caller migration, no-Python and T016 remain open.
