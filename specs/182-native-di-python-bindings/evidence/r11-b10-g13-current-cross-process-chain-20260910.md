# R11-B10-G13 Current Cross-Process Native Chain

使用当前 `build-nac182` 的 requester、Provider、authority、controller 和 ONNX worker
独立可执行文件，重新运行现有 process drivers。driver 只负责 NFD/PIB/TPM 和进程
生命周期；请求、grant、Provider 解包/执行、stream/conversation 状态与 oracle 仍由
C++ 生产可执行文件负责。

## Build identity

```text
DI_NativeRequester       sha256:134ee9680197ecdbaf365f2ffc1ded6114c6513542a388aab6ba35972ef3111a
di-native-provider        sha256:9f9b0ceb6d37c5f00423379f87a4fc434da8bb4e30ecb395ab7c9c421aa720c5
DI_NativeArtifactAuthority sha256:318e0b33335ed143ed57aef1014075831e40017d85c297f518a13ffa1e1d28a
App_ServiceController     sha256:5eb50cc647ac9682ca444c6c170c1618c83ca7c47f90647f717d78ec955260d7
DI_NativeOnnxAssemblyWorker sha256:026b601c700534e086a04b16de30afa2334b2ec57e4d7f54dfc9b3af4f8a53f9
```

## Current runs

| Run | Raw directory | Observed result |
| --- | --- | --- |
| unary | `.codex-tmp/spec182-r11-b10-cross-process-20260910235705` | driver `rc=0`; requester emitted `NATIVE_NUMERICAL_ORACLE_PASS` (`4,0,12`) and `NATIVE_REQUEST_SUCCEEDED`; Provider emitted `NDNSF_DI_GRANT_VERIFICATION` and `NDNSF_DI_EXECUTION_EVIDENCE_OBSERVED` with `realCompute=true`, ORT CPU |
| stream | `.codex-tmp/spec182-r11-b10-stream-20260910235728` | driver `rc=0`; requester emitted `NATIVE_STREAM_ORACLE_PASS` for 8 tokens and `NATIVE_REQUEST_SUCCEEDED`; Provider grant and execution evidence present |
| conversation | `.codex-tmp/spec182-r11-b10-conversation-20260910235745` | first and second requester `rc=0`; checkpoint written at epoch 1; wrong-parent requester `rc=1` with `DI_NATIVE_CONVERSATION_PARENT_MISMATCH` |
| recovery | `.codex-tmp/spec182-r11-b10-recovery-20260910235809` | first turn `rc=0`; Provider deliberately killed; restarted Provider reported `PROVIDER_CONVERSATION_STATE_MISSING`; second turn `rc=1` and wrong-parent `rc=1`, with no duplicate successful continuation |
| replacement | `.codex-tmp/spec182-r11-b10-replacement-20260910235834` | first requester `rc=0`; replacement Provider emitted verified grant and execution evidence for `/recovery/2` with `attempt-2`; driver `rc=0` |
| no-backup | `.codex-tmp/spec182-r11-b10-no-backup-20260910235859` | only Provider fails before admission; requester `rc=1` with `NATIVE_REQUEST_STAGE_FAILED` / `DI_NATIVE_NO_ADMITTED_PROVIDER`; driver records the expected terminal failure |

这些运行是当前 C++ 生产进程链的强行为证据，覆盖 T010/T011 的本地独立进程
边界、continuation/recovery、replacement 与 no-backup fail-closed。driver 本身是
Python 生命周期工具，因此这不是 no-Python harness qualification；维护 caller 全量
迁移、I01--I08/PO 完整矩阵、exact-SIF/多机、T014/T015/T016/T017 仍未关闭。
