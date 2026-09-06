# T002 Shared Native Preparation Closure

**Status**: PASS (focused shared preparation closure)
**Evidence layer**: implemented / wired / executed (focused checks)

## Changes

按 FR-015，`bindNativeRunnerPreparationContext` 为 ONNX 与 native
postprocess 统一绑定 Provider/boot、plan/model/artifact 观察上下文及
唯一 profile 名称；模型 adapter 的 spec 保留自己的计算和状态字段。
生产准备 factory 仅在构造 spec 时选择 adapter，公共证据初始化
只执行一次。原有 profile 文件名前缀与观察开关保持兼容。

`NativeYoloMergeRunner` 算法移至 `cpp/adapters/yolo`；旧
`cpp/ndnsf-di/NativeYoloMergeRunner.hpp` 为兼容 include，不含模型算法。
新增 adapter object target/header 安装规则，同步生产与定向测试源
清单。源码闭包同时收拢此前未提交的 handler 输入/准备接线、native
offer、日志 owner，以及 grant credentials/transport 的生产链接依赖。
既有 Qwen tokenizer/采样与 GPU 观测改动按原边界保留。

## Focused Validation

- R1 Waf `unit-tests,ndnsf-di-adapter-yolo-objects` 构建 PASS（1m8.775s）。
- `NativePreparationContext*,NativeYoloMerge*,NativePreparedRunner*,NativeEpochCoordinatorRejectsCancellationBeforeRunner,NativeEpochCoordinatorRollsBackWhenDeadlineExpiresAfterRunner`：
  **12 cases / 76 assertions PASS**，exit 0。
- 两类 runner 共享上下文的单测检查可信字段覆盖、adapter 字段保留、
  profile 唯一性及 pathless role 的既有 plan fallback；迁移后的实际
  Merge 数值/拒绝回归与既有生成取消/截止接口检查一并通过。
- 原始日志保留在 ignored workspace temporary directory 的
  `spec181-shared-preparation-20260905-r1/`。

## Production Build and Control

维护 `spec180_native_build.py build --jobs 2` 已成功刷新同一工作区
原生程序、库、binding 与身份，exit 0，`SPEC180_NATIVE_IDENTITY_OK`。
manifest SHA-256 为
`10ed17dfe2960b917fb25d8b62487f096c5546c7d2d7cf2c11b219f01a5a5b78`；
原始目录 `spec181-shared-preparation-20260905-native-r1/`。

隔离 P-256 protected Y-B control R1 在 `validate_inputs` 处因
`OUTPUT_ROOT_MISSING` 退出：启动脚本没有建立空输出目录，尚未启动
MiniNDN。这是启动编排错误，无协议结果。原始脚本、日志与 recipient
inventory 保留在 `spec181-shared-preparation-20260905-live-r1/`，
宿主原有 5 个 NFD PID 与运行前一致。

R2 使用新的 `spec181-shared-preparation-20260905-live-r2/`，先建立空
输出目录。维护入口 `_run_live_case_once("Y-B", ...)` 在独立 user/net/
mount/PID namespace 内完成，exit 0、`TERMINAL_RESPONSE_VERIFIED`。
四个 Provider 均记录 `VERIFIED / BEFORE_ASSEMBLY`；三个 ONNX 角色
观察到 `onnxruntime-cpu / realCompute=true`，最终 native Merge 输出
shape `[1,50,6]`，`matched=true`，maxAbsError
`0.0005340576171875`（atol `0.001`，rtol `0.0001`）。

`child-exits.json` 收集全部 7 个子进程：User 为 0，Repository 为
130，其余为 -2；后六者均为监督器请求终止后的退出。全部 `.staging`
为空，case 下没有 `.onnx`、`.weights` 或 `.bin` 明文残留。宿主 5 个
NFD PID 与运行前一致。构建和控制针对当前工作区源身份；目录中其他
预存修改未因此获得资格，亦不把本次定向控制称为正式矩阵或候选资格。

## Remaining Shared Boundary Audit

T007 须继续检查 generation coordinator 到 registered-runner worker
的授权/取消检查传递；现有 coordinator 自身的 stopCheck 通过不等于
排队、计算与发布边界已受同一检查约束。当前调用链核对发现尚未
向该 worker 入口传递 guard，先用定向回归确认再修复公共 owner。
此记录不声称 Qwen 模型资格，也不关闭 T007。
