# NDNSF 本地测试与真实部署保真度审计

日期：2026-07-30  
归档位置：Spec 164 跨模块验证保真度补充审计。  
范围：NDNSF、NDNSF-DI、NDNSF-DistributedRepo 的本地单元测试、MiniNDN
实验、Docker/Compose 测试和 TigerCluster 适配测试。  
审计性质：只读设计与证据审计；本报告不把未执行的测试视为通过，也不修改
测试行为。

## 1. 结论

**总 verdict：BLOCK。**

现有测试足以支持以下较窄结论：

- 大量协议、数据结构、规划、失败状态机和 Python/C++ 绑定具有单元级覆盖；
- 部分实验确实启动 MiniNDN、独立 NFD 和独立进程，不是纯内存模拟；
- Spec 163 的字节级 collaboration 生命周期确实经过真实 NDNSF
  Request/ACK/Selection/Response 网络路径；
- Spec 164 的 artifact harness 确实经过 MiniNDN/NFD、生产者和消费者进程，
  适合验证分段传输、冷读取和有限规模吞吐。

但现有默认测试**不能**支持“NDNSF-DI 已在本地证明可以真实部署并完成分布式
Qwen 推理”的结论，也不能保证所有在 TigerCluster 才出现的问题应当已经被
当前默认本地门禁发现。主要原因不是 MiniNDN 本身无效，而是：

1. 默认 quick/regression 套件排除了多数真实 DI MiniNDN/Qwen case；
2. 被称为端到端的 Spec 163 MiniNDN/Docker 生命周期使用 fake user/provider
   和 byte-sized payload，不加载模型；
3. Qwen MiniNDN harness 默认只生成 1 token、测量 1 次、warmup 0 次；
4. 多数 TigerCluster “integration”脚本使用假 `srun`、假 `apptainer`、
   fixture process map 或静态文件检查；
5. 容器总入口的 `live` 模式明确尚未实现，另两个 live 脚本只验证镜像可启动、
   NFD socket 和重启，不执行完整 DI 业务；
6. 未找到一条真实模型、真实多进程网络测试同时断言同一 request ID 贯穿
   Request → ACK → Selection → operation status → Response；
7. 现有长任务等待主要是固定 wall-clock timeout。未找到以已认证进度事件更新
   `last_activity` 并延长 idle deadline 的生产实现。

因此，TigerCluster 当前既承担最终环境验证，也被迫承担本应由本地门禁承担的
业务路径调试。这会放大镜像发布、模型下载、排队和远程日志诊断成本。

## 2. 审计方法和边界

本审计结合：

- Context Mode：只作检索层。marker
  `CTX_HOST_ACCEPT_20260730_SPEC164_82c5e90bd113` 已被正确项目的
  `user-prompt` 捕获；最终 health 通过，五个 file-backed source 均 fresh，
  七个必需 hook 均 trusted/enabled。复核还修正了一个 guard 误报：
  Codex 会在运行中向 `config.toml` 写入 hook trust hash 或无关设置，因此只有
  host-owned `hooks.json` 的 freshness 才用于决定是否必须重启；
- CodeGraph：检查 `report_operation_status()`、Python collaboration API
  和相关测试调用关系；
- Spec Kit：以当前 Spec 164 计划、任务和已有审计为规范背景；
- GSD：检查长期工作状态和可恢复性；
- Academic Research Suite 的实验审计原则：区分 construct validity、
  internal validity、external validity 和可复现证据。

候选文件盘点排除了 vendored/第三方实验树、build、results 和模型目录。启发式
盘点得到 458 个项目自有候选 runner/test 文件，但该数字只用于发现测试分层，
**不能**作为“458 项均已执行”或各类别精确覆盖率。关键词会造成误分类，所以
最终 verdict 只依赖下列逐文件核对的证据。

本次没有：

- 运行 MiniNDN、Docker、Qwen 或 TigerCluster 作业；
- 下载模型或构建镜像；
- 将历史 PASS marker 当作本次执行证据；
- 修改任何测试逻辑。

## 3. 测试保真度分层

| 层级 | 当前代表 | 真实组件 | 替代/缺失组件 | 可以证明 | 不能证明 |
|---|---|---|---|---|---|
| T0 静态/单元 | `tests/python/test_ndnsf_di_*.py`、Boost unit tests | 当前代码、序列化、状态机 | 网络、进程、模型、GPU | 局部契约和确定性失败语义 | 部署可用性 |
| T1 host-process | HELLO regressions、部分本地 adapter smoke | 真实二进制、可能有真实 NFD | namespace、容器、HPC、模型常缺失 | 本机协议和安全路径 | 跨节点行为 |
| T2 MiniNDN + fake workload | Spec 163 placement/preparation | MiniNDN、NFD、路由、Controller、User、3 Provider | fake payload、fake inference、无 Torch/Qwen/GPU | collaboration 网络生命周期 | 模型发布/加载/生成 |
| T2 MiniNDN + artifact | Spec 164 artifact harness | MiniNDN、NFD、生产者、消费者、分段对象 | 集群存储、GPU、16 GiB 本地证据受资源门禁限制 | 有限规模真实传输 | 集群级并发和存储瓶颈 |
| T2 MiniNDN + Qwen | `NDNSF_DI_LlmPipeline_Minindn.py` | 可选真实 HF/Qwen stage | 默认 1 token、1 次、0 warmup；不在默认门禁 | 单 token 路径（显式启用时） | 完整回答、分布、KV cache |
| T3 本地容器 | OCI/Compose live scripts、Spec 163 bounded Docker | 真实 Docker 镜像/容器、NFD socket 或 fake DI lifecycle | 无 SIF/Slurm/CUDA；多数不执行真实模型 | 镜像可启动、部分服务生命周期 | Tiger 运行语义和真实推理 |
| T4 HPC fixture | `tests/container/itiger-qwen-live/integration` | 脚本逻辑、参数和安全约束 | 假 `srun`/`apptainer`、fixture topology | 适配器契约 | 真实 Slurm/Apptainer/GPU |
| T5 TigerCluster live | 独立 campaign/jobs | Slurm、Apptainer、GPU、共享存储、真实网络 | 无本地替代 | 最终环境与性能 | 不应作为首次业务调试入口 |

## 4. 关键证据

### 4.1 默认 quick suite 不等于真实 DI MiniNDN suite

`Experiments/NDNSF_Run_Minindn_Quick_Checks.py`：

- 第 98 行把一组分支明确标为 `No-MiniNDN quick-smoke`；
- 第 351 行只有显式 `--include-di-minindn` 才加入部分 DI MiniNDN case；
- 默认列表包含 fake/local pipeline；
- `di-llm-transformers-benchmark` 和 `di-llm-qwen-minindn` 有定义，但不在默认
  `all` 路径。

`Experiments/NDNSF_DI_Run_Minindn_Regressions.py`：

- 第 254 行的 `selected_cases()` 中，`all` 仅选择 runtime compatibility、
  fake/local pipeline、app/ONNX/auto-split 和 YOLO 2x2 等子集；
- 第 125、145 行定义的 transformers benchmark 和 Qwen MiniNDN case
  不在默认 `all` 列表。

因此，“quick/all PASS”与“真实 Qwen MiniNDN PASS”不是同一个命题。

### 4.2 当前真实 Qwen harness 的默认验收过弱

`Experiments/NDNSF_DI_LlmPipeline_Minindn.py` 默认值：

- 第 238 行：provider 启动 timeout 20 秒；
- 第 240 行：请求 timeout 60 秒；
- 第 243 行：warmup 0 次；
- 第 244 行：measured request 1 次；
- 第 245 行：最多生成 1 token；
- 第 255 行：可选 expected token IDs。

单 token 对照可以证明一次分布式 forward 的 top-token 一致性，但不能证明：

- 自回归循环可连续工作；
- 完整自然语言答案正确；
- KV cache 在各 stage 的创建、传递、复用和失效正确；
- TTFT、逐 token latency、总延迟和 tokens/s 分布；
- 多请求时 GPU 驻留模型片确实带来 warm-hit 加速；
- 不同 prompt 长度和输出长度下没有隐藏超时或 request ID 漂移。

### 4.3 Spec 163 的 MiniNDN/Docker 是真实网络加 fake workload

`Experiments/NDNSF_DI_PlacementPreparation_Minindn.py`：

- 第 713、725 行通过 MiniNDN `getPopen` 在节点 namespace 中启动进程；
- 第 787 行创建 MiniNDN；
- 第 791–805 行启动 NFD、配置路由并发布各节点前缀；
- 第 863 行启动 `tests/container/placement-preparation/fake_user.py`；
- 第 868、881 行仍使用固定 45/90 秒 process timeout。

`tests/container/placement-preparation/container-entrypoint.sh`：

- 第 58 行运行 `run_fake_di_lifecycle`；
- 第 253 行明确记录 `payload_kind=byte-sized-fake`；
- 第 254 行明确记录 `model_loaded=false`；
- 第 258 行的总 PASS marker 因而只代表有界 fake lifecycle。

`fake_provider.py` 第 147 行发布
`SPEC163_FAKE_INFERENCE_OK`，没有模型加载路径。该测试仍然有价值：它验证真实
NDNSF collaboration assignment、选择、安全载体和最终响应；但它不能被标为
真实 Qwen 或模型部署测试。

### 4.4 容器测试的 live 含义不统一

`tests/container/run.sh`：

- `offline` 只执行 Python unittest discovery；
- `live` 要求 `NDNSF_CONTAINER_LIVE=1`，随后明确报
  `no live tests are implemented in Spec 108 Phase 1-2` 并退出。

`tests/container/integration/test_oci_cpu_smoke.sh`：

- 是真实 Docker build/run，但容器内只执行 `/bin/true`；
- 它证明 rootfs、入口和镜像可启动，不证明 NFD/NDNSF-DI 业务。

`tests/container/integration/test_compose_cpu_node.sh`：

- 是真实 Docker Compose；
- 只断言 NFD socket、容器重启和目录存在；
- 没有执行 Request/ACK/Selection/Response 或模型推理。

Spec 163 的 `tests/container/placement-preparation/run.sh` 更强：单容器中运行 NFD、
Controller、Requester 和三个 Provider，并限制为 4 GiB memory/5 GiB swap；
但其 workload 仍是上述 fake byte payload。

### 4.5 多数 Tiger “integration”是适配器 fixture，不是远程作业

例如：

- `test_network_scripts.sh` 使用 fixture process map，并在临时目录创建假
  `srun` 和 `nfdc`；
- `test_rootless_build.sh` 创建假 `podman`、`buildah` 和 `apptainer`；
- `test_release_pipeline.sh` 静态检查 Dockerfile/workflow，并创建假
  `apptainer` 生成伪 SIF bytes；
- `test_packaged_security_contract.sh` 以 fixture process map 运行 contract mode。

这些测试能发现参数、路径、安全挂载、错误传播和证据 schema 回归，但不能证明：

- Slurm 实际分配三节点；
- Apptainer 能在 Tiger 节点拉取/运行候选 SIF；
- CUDAExecutionProvider 真正使用 GPU 且 CPU fallback 为 0；
- 节点间 NFD 路由、共享文件系统和 GPU 内存行为正确。

### 4.6 request ID 覆盖仍是分层而非完整链路

已确认：

- `test_ndnsf_python_service_response_binding.py` 第 16 行验证 request ID
  跨 pybind round-trip；
- `test_ndnsf_collaboration_operation_status.py` 使用 `SimpleNamespace`
  验证 status snapshot 的 request ID/provider/digest 绑定；
- fake Docker user 使用固定 `spec163-docker` request ID 并等待最终 response；
- generic MiniNDN performance analyzer能按 request ID 匹配 ACK、Selection 和
  Response。

但本次审计**未找到**一条同时满足以下条件的测试：

1. 真实模型；
2. MiniNDN 或真实多进程网络；
3. 从初始请求提取唯一 request ID；
4. 对每个 ACK、Selection、每条 operation-status/progress 和最终 Response
   逐项断言完全相同的 request ID；
5. 注入一次 mismatch 并证明接收方拒绝。

所以目前可以说“各层分别有 request ID 检查”，不能说“完整真实模型生命周期
已经证明 request ID 永远一致”。

### 4.7 长任务仍由固定 timeout 主导

已确认的例子包括：

- Qwen MiniNDN 请求默认 60 秒；
- Spec 163 fake user 的 invocation/result 均为 15 秒；
- Spec 163 MiniNDN 子进程为 45/90 秒；
- Slurm adapter
  `packaging/ndnsf-di-container/lib/adapters/slurm_apptainer.py:83`
  为 `wait(job_id, timeout=600, poll=5)`。

代码中存在 progress/status 数据类型和 `report_operation_status()`，但本次语义
扫描未找到 DI/DistributedRepo 生产路径维护：

```text
last_authenticated_activity
idle_deadline = last_authenticated_activity + stall_budget
absolute_deadline = request_start + hard_cap
```

也未找到每次有效 progress 更新 idle deadline、而无进展时触发 STALLED 的完整
实现。对大模型发布而言，仅把 600 秒改成更大的固定值不能区分“正在缓慢传输”
和“已经死锁”。用户此前提出的进度监测方向是正确的，但必须同时保留 hard cap，
并且只有经过身份、request ID、attempt/epoch/sequence 验证的进度才能续期，避免
恶意 heartbeat 无限占用资源。

### 4.8 Spec 164 的本地传输证据比 DI 默认门禁更真实

`Experiments/NDNSF_DistributedRepo_Artifact_Minindn.py` 确实：

- 创建 MiniNDN/NFD；
- 分别启动 producer/consumer；
- 使用分段 Data 和 cold consumer；
- 读取独立 result/progress 文件并检查进程退出；
- 支持 raw NDN 和 repository subject。

`Experiments/spec164_artifact_campaign.py` 第 45 行要求正式 rate-over-time
窗口至少 60 秒，并定义 warmup/repetition。其设计目标包含 1 MiB、64 MiB、
1 GiB 和 16 GiB，但当前已保存的本地正式证据只接纳到 64 MiB r1/c1；1 GiB
和 16 GiB 受本机资源门禁限制，SQLite/共享存储在集群级并发下的表现也未证明。

因此 Spec 164 可以获得“真实 MiniNDN 有限规模传输 PASS”，不能外推为
“TigerCluster 大模型仓库已达链路带宽”。

## 5. 为什么问题会拖到 TigerCluster 才发现

这是测试分层和 PASS 语义的问题，而不是简单的“本地是否运行过 Docker”：

1. 默认入口优先跑最快的 unit/fake case；
2. 真实 MiniNDN/Qwen case 需要额外 flag 或根本不在 `all`；
3. 即使显式跑 Qwen，默认也只有 1 token/1 request；
4. 本地 Docker smoke 证明镜像和协议载体，但不装载模型；
5. HPC adapter tests 用 fixture 模拟调度器和容器运行时；
6. 只有 Tiger live 首次把模型仓库、SIF、Slurm、多节点 NFD、CUDA、显存和长耗时
   放在同一条链路中。

因此，之前本地 PASS 并不与 Tiger 失败矛盾；它们验证的不是同一个系统构造。
真正的问题是测试名称、默认集合和验收报告没有把这种构造差异声明为硬边界。

## 6. 必须建立的门禁

### Gate A：统一测试元数据

每个测试和每份结果必须声明：

```yaml
fidelityTier: T0|T1|T2|T3|T4|T5
realComponents: []
simulatedComponents: []
network: none|host-nfd|minindn|docker|cluster
container: none|docker|compose|apptainer
model:
  name: ""
  revisionOrHash: ""
  realWeights: false
hardware:
  gpuRequired: false
skipIsFailure: true
command: ""
sourceRevision: ""
```

一个高层 aggregate gate 不得把 SKIP 当 PASS，也不得用 T0/T1/T4 的结果满足 T2、
T3 或 T5 的 requirement。

### Gate B：默认 MiniNDN DI 真实最小模型门禁

默认门禁应运行最小 Qwen3 模型，而非 fake payload：

- 独立 NFD、Controller、User、至少 3 Provider；
- 真实 Request → ACK collect → graph-aware split/placement → Selection；
- 真实 artifact publish/fetch/verify/load；
- 完整生成至少 16–32 tokens，而非 1 token；
- 至少 2 个 prompt；每个 warmup 1 次、测量至少 3 次；
- 保存完整答案、TTFT、逐 token latency、总延迟和 tokens/s；
- 断言 CPU/GPU 配置符合本地能力，不能静默 fallback；
- 断言 request ID、model name+hash、attempt、epoch、selection digest 全链路一致。

若本机没有 GPU，可以使用 CPU 最小模型证明业务正确性；GPU 性能和 CUDA
正确性留给后续门禁，但结果必须明确标为 CPU。

### Gate C：真实本地 Docker 业务门禁

候选镜像必须执行与 Gate B 相同的最小模型业务，而非 `/bin/true`、NFD socket
或 fake payload。可继续使用 `--memory=4g --memory-swap=5g`，但模型大小应确保
不会因本机资源不足而把基础业务错误隐藏成 OOM。

### Gate D：进度驱动的长任务控制

模型发布、下载、验证和加载使用双 deadline：

- idle/stall deadline：只在收到新、合法、单调递增的 progress 后续期；
- absolute hard deadline：任何 heartbeat 都不能越过；
- progress 包含 request ID、operation ID、attempt、epoch、sequence、bytes
  completed/total、当前阶段和时间戳；
- duplicate/reordered progress 不续期；
- producer 退出、无字节增长或 hash mismatch 立即失败；
- 测试覆盖慢但持续进展、停止进展、伪造 heartbeat、重连恢复和最终 hard cap。

### Gate E：TigerCluster 分级升级

只有 Gate A–D 全部通过后才发布：

1. TigerCluster 单节点/小模型 capability smoke；
2. 三个 RTX 5000 节点，每节点一个 stage，小 Qwen3，完整多 token 生成；
3. 确认模型片发布、复用和 request ID 一致后，才升级大模型；
4. 大模型 campaign 保留 5 个真实 prompt、每个 warmup 1 次、测量 5 次、
   最多 64 tokens 及完整延迟分布。

Tiger 失败时应先判断是 T2/T3 也能复现的业务问题，还是只属于 T5 的 Slurm、
Apptainer、CUDA、共享存储或真实链路问题。

## 7. 学术有效性风险

### Construct validity

当前“PASS”经常测量协议 marker、top token 或脚本 contract，却被解释为完整分布式
推理。需要把“被测构造”写入结果元数据并禁止跨层外推。

### Internal validity

固定 timeout 把慢进展和死锁混为一类；fixture scheduler 可能掩盖真实 Slurm
状态转换；默认单请求无法暴露缓存、并发和 sequence race。

### External validity

CPU MiniNDN、单容器和 fake bytes 无法外推至三节点 GPU、SIF、共享存储和大模型；
反之，Tiger 上一次成功也不能外推至不同模型 hash、节点类型或缓存状态。

### Reproducibility

结果必须绑定 source revision、image digest、model hash、命令、拓扑、资源限制、
随机种子、warmup/measurement 次数和所有 SKIP。仅有 `*_OK` marker 不够。

## 8. 审计中观察到的工具失败

三次用于批量读取脚本的 shell `for` loop 被 Context Mode 注入的
`NODE_OPTIONS=... for ...` 语法破坏。后续使用 Python 文件遍历完成同一只读
采集。该失败没有被当作测试失败或静默忽略，也没有影响上述逐文件结论。

较早的 2059 个候选文件统计混入 `Experiments/gRPC` 等 vendored tree，已判定
无效并废弃；本报告不使用该数字。

## 9. 最终判定

- **NDNSF 单元/契约层：PASS（仅限该层）。**
- **通用 NDNSF MiniNDN 网络路径：PASS WITH LIMITATIONS。**
- **Spec 163 collaboration planning 的 fake lifecycle：PASS WITH
  EXPLICIT FAKE-WORKLOAD LIMITATION。**
- **Spec 164 artifact transport 的有限规模 MiniNDN 路径：PASS WITH
  EXPLICIT SIZE/CONCURRENCY LIMITATION。**
- **默认本地测试足以证明 NDNSF-DI 真实 Qwen 分布式推理：BLOCK。**
- **默认本地测试足以防止业务错误首次出现在 TigerCluster：BLOCK。**
- **用 TigerCluster 作为主要调试入口：REJECT。**

最值得先做的不是继续大模型实验，而是实现 Gate A–D，并把默认 aggregate runner
改成“真实最小 Qwen3 MiniNDN + 真实本地 Docker”失败即阻断；随后才恢复
TigerCluster 小模型三节点验证。
