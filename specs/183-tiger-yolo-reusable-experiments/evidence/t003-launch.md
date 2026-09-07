# T003 Shared Launch Boundary Checkpoint

Date: 2026-09-06
Status: PARTIAL_IMPLEMENTATION / FOCUSED_TESTS_PASS / runtime NOT_RUN

## Changes

共享`runtime/baseline.py::container_command`增加显式`gpu`/`gpu_device`和可选只读`/artifacts`挂载。GPU参数只接受一个数字设备或完整GPU UUID；不从主机ambient环境隐式取得模型运行设置。这里仅验证语法，不证明设备属于当前allocation；设备映射、CUDA实际执行和CPU fallback检查仍由T003 worker、T006及实际preflight负责。

所有固定和可选挂载路径统一拒绝相对路径、父目录穿越及bind分隔符。容器继续固定`--pwd /bundle`，每角色独立HOME，不给普通运行挂载整棵私钥目录；不提供任意环境变量覆盖接口。`container_env`额外移除CUDA/NVIDIA/ORT宿主设置。CPU调用默认不增加GPU或模型挂载。`Processes.start`支持显式cwd，以真实子进程验证相对artifact读取。

## Evidence

- 新测试先运行：25 failed / 1 passed；暴露缺失GPU/model/cwd参数、未检查可选挂载、未移除GPU环境。
- 修复后：`python3 -m pytest -q Experiments/TigerCluster/tests/test_yolo_runtime.py Experiments/TigerCluster/tests/test_baseline.py Experiments/TigerCluster/tests/test_worker_lifecycle.py --junitxml=Experiments/TigerCluster/results/spec183-t003-launch-r1/junit.xml`。
- exit0，84 passed in 2.60s；其中26项新增launch检查，既有CPU及实际有限进程/后代清理回归同时通过。JUnit为忽略输出，不进Git。
- SSH只读登录成功，hostname=`itiger`，user=`tma1`。没有构建、上传SIF、申请allocation或运行模型。

## Remaining

T003保持unchecked：还需在真实YOLO worker调用这些参数，校验角色PIB互不共享、端口/cwd/package路径合同、allocation设备归属，以及整组有界启动/清理和peer readiness。当前共享`Processes.close`保留原逐进程等待行为，尚不满足Spec183全组30秒清理预算。没有实现另一套supervisor，也不能把命令组成测试称作CUDA/SIF资格。

发现T002最终边界/receipt校验依赖T004/T006，已修正plan/tasks内部实现顺序；未减少验收条件，T007仍要求T002–T006全部闭合。下一步是共享生命周期和worker/profile接线，再闭合T002实际外部调用门。
