# R11-B9-G7 T016 PO-001 Current Native Stream

本批用当前 `build-nac182/integration-tests` 重跑一个已冻结的 PO-001 native
stream case。owner 由 `Experiments/NDNSF_DI_NativeClosure_Minindn.py` 创建真实
MiniNDN requester/provider namespace；canonical runner 使用 bubblewrap、strace、
staged ELF 依赖和受控进程组，业务进程仍是 C++ `integration-tests`，没有 Python
requester、Python provider 或 Python oracle。

## Scope and boundary

| Lane | Result |
| --- | --- |
| production entry/callers | `integration-tests` → `Spec170NdnsfDiCoreFlow/Spec182R10B37RealProviderNativeStreamRequest` |
| implementation/wire | 当前 native requester/Core/Provider stream fixture；真实 MiniNDN owner namespace 仅负责节点与 NFD 上下文 |
| test/harness/oracle | C++ case 输出 `SPEC182_NATIVE_DI_REQUEST_RESULT_OK`；runner 观察 identity/process-tree/namespace/exec-map/endpoints/business-oracle/cleanup 七类证据 |
| build/source closure | executable digest `sha256:ee4c2c34c7977fd4617fe6e5b2bd8512d60da1002c24a1440abe73816f4348ad`；staged shared-library closure 通过，未发现 Python/libpython 字节或 trace marker |
| migration/evidence | PO-001 单 case 通过；16 个 maintained callers、legacy zero-use、完整 I01--I08/PO matrix、T015/T016 其余门与 T017 仍开放 |

## Commands and durable outputs

```text
env PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin \
  python3 Experiments/NDNSF_DI_NativeClosure_Minindn.py \
  --manifest .codex-tmp/spec182-t016-r14/registration-manifest.json \
  --output .codex-tmp/spec182-t016-r14-owner-current-20260911/result \
  --execute-owner \
  --runner-manifest .codex-tmp/spec182-t016-r14/runner-manifest.json \
  --runner-case PO-001-stream
```

输出目录为 [.codex-tmp/spec182-t016-r14-owner-current-20260911/result](../../../.codex-tmp/spec182-t016-r14-owner-current-20260911/result)。
`result.json` 和 `runner-result.json` 均为 `PASS`；runner return code 为 0，
`durationMs=5587`，`timedOut=false`，`evaluation.failures=[]`，七类 evidence
全部出现。C++ stdout 保留于
`result/closure-run/stdout.log`，包含 `SPEC182_NATIVE_DI_REQUEST_RESULT_OK`；
strace 保留于 `result/closure-run/trace.txt`；真实节点身份保留于
`result/node-context.json`。

该结果关闭的是当前构建下 PO-001 native stream 的隔离正例。它没有执行完整
I01--I08/PO-001--PO-014 matrix，也没有证明 maintained caller migration、旧
Python runtime 零调用、exact-SIF/多机交付或 T017 handoff，因此 T016 parent 继续
保持 `PARTIAL`，R11-B9 继续等待后续资格批次。
