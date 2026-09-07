# Native Model Route Correction

Date: 2026-09-07
Status: SOURCE_VERIFIED / FOCUSED_PASS / runtime NOT_RUN

## Finding And Owner

T003 新增 worker 曾强制每个模型 Provider 提供 `model_artifacts` 并挂到
`/artifacts:ro`，T004 后续任务因此要求生成 role-only projection。核对真实
应用后发现该目录不是当前 native YOLO 的输入接口，属于无用且增加歧义的
实验配置，必须移除，而不是再实现一套模型切分/复制流程来满足它。

确认的既有 owner：

- `ndnsf_distributed_inference/adapters/yolo/adapter.py` 的 canonical publisher
  `ensure()` 调用 `publish_encrypted_artifact` 发布 graph、external initializer
  和 root；返回实际 `artifact_fetch_data_names_by_role`。无 publisher 的离线
  adapter 路径不能用于正式 Spec183运行。
- `examples/DI_NativeProviderExecutable.cpp` 从 `--artifact-cache-dir` 取得
  `assemblyCacheDir`，在真实 Selection projection/context 中调用
  `prepareNativeCanonicalOnnxRole`；Merge 走已有 native postprocess。
- 旧 `jobs/spec180/render-tiger-yb-args.py` 也为每 Provider 传独立 native cache，
  而 canonical package 是 User 参数。旧 renderer 仅作接口证据，不复制其
  hard-coded identities 或旧运行结论。

因此正常 Spec183 保持发布方读取 canonical package、Provider 经安全 NDN
获取并在自己的可写 cache 中组装。无需每 Provider 人工 stage 模型，更不能
把含完整模型参考输出的包放进所有角色可见的脚本 bundle。cache argv 和发布方
专属挂载仍需 T004/T005 的实际接线，T007 和后续真实模型测试验证数据流。

## Fix And Focused Evidence

移除 `NodeRuntime.model_artifacts` 参数及强制目录检查，Provider 命令不再带
`/artifacts` mount。保留共享 baseline 的通用 `container_command(artifacts=...)`
接口，因为其他消费者仍可能使用；没有修改 Core/DI/SIF 或绕过其 artifact 鉴权。

新 tracer 在省略该参数时先失败 `missing required keyword-only argument`。
修正后对四个 Provider、两种 rank 分别通过真实短进程/fake-Apptainer边界测试，
确认无模型旁路挂载。原“缺 model projection 目录”断言由“旧参数明确不支持”
取代，不删除工作目录/身份/输出别名/GPU/清理相关保护。

```text
python3 -m pytest -q Experiments/TigerCluster/tests --tb=short \
  --junitxml=Experiments/TigerCluster/results/spec183-native-model-route-r1/junit.xml
```

Exit0，**239 passed in 12.09s**；其中 worker 为31项，先前为27项。
没有真实 ONNX/NDN/SIF/CUDA 执行，不证明模型路由已在新 launcher 端到端接通。
T003组件经改动后聚焦回归仍通过；T004/T005/T007及所有正式资格仍未完成。
早期 `t003-worker.md`/`t004-cli-journal.md` 中 role-only projection 后续安排是
历史设计，本修正覆盖它，原始测试数/证据不改写。
