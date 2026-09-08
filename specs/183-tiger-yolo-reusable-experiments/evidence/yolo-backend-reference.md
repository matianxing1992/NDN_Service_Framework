# Same-Model Backend Reference — 2026-09-08

用户要求基于已通过的NDN小例子执行YOLO真机推理并修复问题。独立参考是定位
模型/CUDA问题的前置诊断，不替代四Provider的NDNSF-DI请求/鉴权/数据路径。

## Diagnostic Source Audit

范围：`apps/yolo_reference_probe.py`与`jobs/yolo/reference.sbatch`，均归属
Experiments/TigerCluster。使用现有canonical graph/weights和NumPy参考owner；
不下载/导出新模型、不更换误差标准、不修改NDNSF安全机制。

- graph/weights先核对已有manifest摘要，external initializer只从绑定权重对象
  的合法offset/length取值，不读取exporter任意路径。参考owner核对fixture/oracle。
- 两次真实ORT执行（1warmup+1measured），保留每次数值对照和ORT kernel profile。
  CUDA要求实际CUDA kernel，禁用EP fallback并拒绝观测到的CPU执行；失败不降级PASS。
- 单节点rtx_6000:1、2CPU、4GiB、五分钟allocation，容器调用120+10秒上限。
  staging前验证脚本/小文件checksum与容量/fsync，节点暂存同一历史SIF并核对
  b6710fd6完整摘要；模型/代码只读，结果持久保存在project，scratch只清理本run。
- Slurm GPU选择显式传给容器，实际Apptainer要求1.5.3（允许打包后缀），记录
  host/GPU/driver。不在login节点执行模型、NFD或构建。
- 本地CPU实际路径PASS；冻结远端8文件checksum、shell syntax和入口help已PASS。
  此审计仅允许有界backend reference；T007 N1/N2/N4不会因此关闭。

## Current Evidence

Raw root: `Experiments/TigerCluster/results/yolo-layered-20260908/`。
本地`reference-cpu-1`：ORT1.20.0，真实CPU kernel，warmup/measured两次输出
均[1,50,6]，maxAbsError0.0003662109375，atol0.001/rtol0.0001，两次matched。
容器为现有历史SIF；本地缓存执行不解决其旧内容身份疑问。Tiger将核对暂存摘要。
GPU209982：itiger02，RTX6000Ada，ORT1.20.0真实CUDA kernel，SIF校验及清理通过，
但数值FAIL。置信度过滤后49行而非50，CPU第49行分数0.0010087192，GPU为
0.0009920001，跨越固定0.001阈值。保留失败，不增加容差或更换输入。

GPU209983：同节点/卡型/模型/输入，仅显式设置`use_tf32=0`并记录实际provider
options；两次输出均[1,50,6]、matched、maxAbsError0.00042724609375，实际
kernel仅CUDA，Slurm/batch COMPLETED0:0，24秒，scratch删除。正式调用约5.82ms，
只是一条诊断测量，不构成性能统计。8文件checksum和逐节点SIF校验保留。
raw位于collected-209982/与collected-209983/；project对应
`/project/tma1/ndnsf-di/results/yolo-reference-20260908a/`和`...08b/`。

ONNX Runtime [CUDA文档](https://onnxruntime.ai/docs/execution-providers/CUDA-ExecutionProvider.html#use_tf32)
说明默认TF32与关闭选项；实际对照支持此精度设置是本次误差原因。Python参考
与原生`makeSessionOptions`同步显式use_tf32=0（原生使用CUDA options V2）；
原生改动仍须编译、装入外置app并验证NDNSF-DI链路，不把209983记为native PASS。

同步修复验证：原生OnnxRuntimeModelRunner.cpp以实际ORT头文件开启
NDNSF_DI_ENABLE_ONNXRUNTIME_CPP通过g++语法检查；尚未链接/发布新Provider。
147个受影响参考/原生观测/保留结果组件检查通过；新增2个测试拒绝TF32开启及
旧清单缺少精度策略，均通过，不重复前147项。参考sessionOptions新增
cudaUseTf32=false，使旧未知精度记录不能冒充新参考。证据分别在
tf32-reference.xml、tf32-policy-rejection.xml与preflight/native-tf32-syntax.log。

外置应用初步检查：DI当前Python包可在SIF中import；User第一次挂在`/app/yolo`
因`parents[4]`依赖过浅布局失败，修正打包布局为`/app/repo/examples/python/...`，
保持既有源owner而不改应用算法。初次工具调用错误使用Tiger的`/usr/bin/apptainer`
在本机失败（本机为`/usr/local/bin/apptainer`）；保留原log，使用主机各自明确路径。
这些均非MiniNDN交付失败，完整外置应用/NDNSF真机推理仍在进行。
