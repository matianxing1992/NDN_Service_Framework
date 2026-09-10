# Two-node Runtime Baseline

## Objective And Boundary

提供可复用的启动、身份、路由和清理实现，并在两个实际Tiger节点上完成签名Data往返与真实NDNSF服务调用。固定现有r119 SIF，新增应用包只读挂载；不重建/替换镜像原生库，不修改合并审查工作树。Python探针是既有API的应用调用者，不增加DI规划或框架协议。

本实验是独立于Spec182原生迁移的基础设施工作，不关闭其T001--T017。完成要求是静态审查与相关单测PASS、精确SIF本地集成PASS、双节点实际运行和复用消费者PASS；不能用端口探测、mock、结构检查或旧实验结果替代。

## Ownership And Entry Points

| Path / symbol | Contract |
| --- | --- |
| `runtime/baseline.py` | 新增共享配置、运行目录、容器命令、进程所有权、路由和结果收集。所有新实验共用，不修改冻结Spec175/180脚本。 |
| `runtime/identities.py` | 容器内离线创建每角色独立PIB/TPM并由单一run-root签发；只分发各角色私钥和公开证书，根私钥不导入角色HOME。 |
| `runtime/worker.py` | 每节点一个worker；使用baseline启动NFD/应用、安装路由、等待真实就绪、记录每阶段结果并在finally清理子进程。 |
| `apps/ndn_probe.py` | 双向唯一名称Interest/Data、在线获取并验证叶证书、RSA签名验证、错误签名负例；不使用空validator。 |
| `apps/service_probe.py` | 调用镜像内ServiceProvider/ServiceUser，固定echo响应；选择回调记录实际ACK/选择。Controller直接使用镜像已有C++ App_ServiceController。负例覆盖无目标权限与原生错误trust-root。 |
| `profiles/two-node.json` | 唯一双节点CPU配置；资源、端口、超时、SIF路径/哈希和应用参数显式定义；未知字段拒绝。 |
| `jobs/baseline/submit.py`, `run.sbatch` | 单一prepare/submit/local入口。baseline与后续service-echo消费者都调用共享runtime，不复制启动器。prepare不提交；submit只使用已核对的配置和应用清单。 |
| `tests/test_baseline.py` | 配置拒绝、命令/身份隔离、结果判定、进程清理和假绿灯回归；不启动Slurm。 |

## Interfaces And State

实现时公开入口保持英文docstring，解释输入来源、失败边界及资源所有权；普通循环变量属于LOCAL_DETAIL。

- `load_profile(path) -> dict`：读取唯一schema；校验nodes=2、CPU资源、端口/超时范围、绝对SIF路径、64位SHA-256及允许workload；错误在创建作业前抛出。
- `digest(path) -> str` / `write_json(path, value)`：流式SHA-256；同目录原子替换JSON，避免barrier读到半文件。只写当前新run目录。
- `container_command(profile, bundle, home, public, output, argv) -> list[str]`：cleanenv、独立HOME、只读应用/公开配置、角色输出可写；显式容器Python和原生库来源；不bind宿主库、venv或整个工作区。
- `Processes.start(name, argv, env) -> Popen` / `close() -> list[dict]`：独占新进程组与日志；逆序TERM、限时等待、必要KILL并wait；只操作本对象创建的PID；清理失败阻止PASS。
- `wait_json(path, timeout, children)`：单调时钟deadline；等待期间检查本worker子进程健康和共享失败标志；超时不是协议拒绝。
- `configure_routes(...)`：通过精确SIF的nfdc连接当前节点专属socket，安装到另一节点的TCP face和run namespace路由、SVS multicast策略；持久化实际face/route快照。
- `collect(...) -> dict`：要求两个实际不同主机、同一SIF/bundle/profile身份、必需case全部精确符合oracle、worker exit0及全部清理完成。遗漏、异常、超时、未知case、篡改身份均FAIL。

Spec110 allocation topology 的 `identityRef` 是只读输入，不能直接复用为运行时 HOME。
启动器会在每个执行节点的作业 scratch 中创建进程专属 HOME，复制该角色的 `.ndn`，并
显式设置 `NDN_CLIENT_PIB`/`NDN_CLIENT_TPM`；NFD 也清除继承的 keychain 变量。身份源或
PIB/TPM 文件缺失会在 `exec` 前失败，避免 MiniNDN 预置 HOME 掩盖多机部署错误。
同一启动器要求显式传入各节点可见的 `--workdir`，并在进程脚本中先 `cd` 到该目录，避免
相对配置和模型路径解析到 Slurm 提交目录。
多节点启动时，supervisor 会先在每个目标节点用 `srun` 检查 `--workdir`，再把生成的
launcher 复制到该节点的作业 scratch 后执行；因此 evidence/提交节点路径未挂载到某个
计算节点会在 NFD 启动前失败。Provider 还要求单值 `CUDA_VISIBLE_DEVICES` 与
`nvidia-smi -i` 返回的 GPU UUID 精确匹配，不能只依赖全机 UUID 列表。
同一节点的 NFD、Controller、User 和多个 Provider steps 使用
`--overlap --exact --ntasks=1 --cpus-per-task=1`；不能使用 `--exclusive`，否则
一个长生命周期 step 会独占整节点并阻塞其余角色。
NFD 配置同样写入该作业 scratch；process map 中固定的 `/tmp` 配置参数会在 launcher 内
重绑定到 scratch 副本，避免并发作业复用旧配置。

`profile`为只读实验契约；`runId`为严格字母数字/连字符标识，决定`/example/tiger/<runId>`命名空间。`mode`仅为`local`或`slurm`，本机双实例只能给LOCAL_PASS。`workload`为`baseline`或`service-echo`；后者是复用同一设施的第二次真实服务实验，不代表YOLO/Qwen已迁移。

`run.json`记录schema、状态、开始/结束UTC、mode/workload、源码基线及逐文件hash、SIF/profile哈希、Apptainer/NDN/框架版本、节点/路由、case结果、worker退出和清理结果。私钥、bootstrap token及PIB/TPM不进入结果包或Git。`private/`只在0700新run根内暂存，退出时删除；公开证书与其摘要留存。

## Execution And Independent Oracles

1. 验证SIF和应用包身份，创建run专属公共配置及私有角色HOME。
2. 两节点各启动一个NFD，使用独立socket和配置端口。就绪依据nfdc实际响应；安装双向run namespace路由。
3. 节点A/B各运行raw producer；必须收到对应注册成功，再运行双向consumer。证书通过网络获取、用指定root验证；Data内容包含run nonce和远端节点rank。故意破坏签名的Data必须触发签名拒绝，超时不算负例通过。
4. Controller在A、Provider在B、User在A。启动镜像C++ Controller后，必须通过真实签名PUBPARAMS请求确认authority就绪，再启动Provider；User必须拿到目标服务permission，再发出固定请求。ACK候选来自真实native回调，Provider身份/状态必须匹配；最终payload与独立固定字符串一致，Provider仅记录合法调用。r119普通V2绑定不填充认证元数据，记录NOT_EXPOSED，禁止宣称这些字段已验证。
5. 合法签发但无服务权限的User先完成合法CONTROL请求，再执行ECHO必须被权限边界拒绝，Provider不得执行该请求。错误trust-root另有raw Data负例和原生NDNSF负例：同一合法User换错根时必须观测明确的原生认证拒绝，单纯超时/空权限不构成通过。r119在更早的PUBPARAMS验证失败时会abort；只允许该独立负例出现exit134/SIGABRT且日志包含Fetched public parameters cannot be authenticated，完整记录其非优雅失败限制。若到达permission阶段，则必须exit0、空权限且有PermissionResponse Data validation failed。其他崩溃/退出不得算通过。
6. 无论成功、部分启动或失败都清理本run进程；两个worker结果与进程退出一致才汇总。后续service-echo使用同一runtime再运行一次并记录其复用身份。

## Work Units And Checkpoint

- [x] B001 Shared runtime and identity configuration：实现、源码审查、相关unit；运行证明留B003。
- [x] B002 Probe applications and consumers：两个真实API应用、双节点profile、提交入口/collector、文档；源码审查与unit。
- [ ] B003 Runtime verification and reuse：最终unit、精确SIF本地集成、真实双节点baseline与service-echo、必要负例/清理，保留完整身份与结果。

2026-09-06：SSH只读检查PASS（itiger/tma1），远端r119文件存在；尚未核对远端hash或提交作业。镜像内ServiceController/Provider/User接口已读源码；安装头文件/工具链不用于编译本探针。前次unsquashfs的`-cat`不受本机版本支持，改用容器内只读cat；导入路径查询以镜像实际模块布局为准。这些是能力检查，不是实验验收。

2026-09-06 B001/B002：新增runtime/apps/profile/launcher/collector完成；源码审查修正Controller异步就绪、原生拒绝负例必须有permission日志及合法control请求、清理进程组、提交前后hash一致和宿主环境注入隔离。`python3 -m pytest -q Experiments/TigerCluster/tests/test_baseline.py` **24 passed in 0.32s**；含错误profile、伪造PASS、超时冒充验签拒绝、非零退出、残留私有状态与进程所有权。尚未运行B003，不能据此声明实际密钥/API/双节点通过。本地/远端SIF均为`b6710fd696a7f962f67f67a54278d92a15babb856c4af428a3f04ba54dc83285`；远端Apptainer实际版本为`1.3.4-1.el9`，本机为`1.3.4`，本地模式显式记录运行环境差异。

## Usage

2026-09-06 Consolidation Review：用户暂停实验后，将当前开发工作并入Experimental。
静态复审发现collector未充分核对ACK/provider/request/selection及wrong-root
boundary/exit/termination组合，有限应用退出也未核对遗留进程组。
新增collector回归在旧实现上 **19 failed / 13 passed / 20 deselected**，
首边界为伪造或矛盾记录被接受；raw为仓库根临时目录
`merge-20260906/tiger-baseline-collector-red-r1/`。生命周期修复新增真实短命父进程/
长命子进程单测；最终结果在本段后续记录。B003和Local R8 FAIL保持，未重跑SIF/Tiger。

Consolidation Review Closure：collector要求真实ACK的provider/status/service/requestId
及实际selectedProviders精确关联；原生错根负例仅接受契约规定的
boundary/exitCode/termination组合，并拒绝类型混淆。探针新增字段已核对r119
构建归档中的实际wrapper/native绑定。有限应用在成功、非零退出、允许的134退出
或超时后均检查所属进程组；残留子进程被清理且forced=True，阻止PASS。
两个审查者交叉复核完成，无剩余本轮控制性发现。

最终命令 `python3 -m pytest -q Experiments/TigerCluster/tests`：
**58 passed，exit0，0.96s**（runner 1.237s），包含52项collector/config/runtime
及6项真实有限进程清理回归；完整raw/receipt保存在审查工作树
`.codex-tmp/merge-20260906/tiger-baseline-final-r1/`。
此前29项baseline与定向52/6项结果保留。本次只完成当前开发源码的静态修复与单测；
B001/B002更新为上述证据，B003保持未完成，Local R8失败不回写为PASS。

Local R1首边界：宿主Python不支持str.removeprefix，profile版本解析exit1，尚未创建身份/NFD或执行协议；原始命令输出保存在`results/local-baseline-r1/preflight-error.txt`。改为兼容Python3.8的显式前缀校验/切片，下一次使用新run-id。

Local R2：`results/local-baseline-r2/run.json`与identity.log保留身份准备失败；ndnsec cert-install -N不能向不存在的root identity安装证书。角色不需要在PIB中拥有root，validator已从只读root.cert加载信任根，删除该多余安装步骤；根私钥不分发的设计不变。未启动NFD，private目录已清除。

Local R3：两NFD在添加内部`/localhost/nfd` FIB时失败（error10021，validator未完成callback），未进入网络探针。源码复审发现INFO配置把多个无值privilege及allow项写在一行，不能保持原模板的节点语义；所有生成INFO配置改为逐行key/value/列表条目。失败日志和两个已reap进程记录保留在`results/local-baseline-r3/`，下一次运行验证配置修正。

Local R4：多行配置修正后两端NFD和双向route命令PASS，raw producer因类名误用KeychainSqlite而导入失败，镜像实际符号为KeychainSqlite3。修正名字，并将两个应用的完整导入加入NFD之前的runtime preflight；`results/local-baseline-r4/`保留两个节点的route输出与失败/清理记录，尚未获得Interest/Data结果。

Local R5：新增preflight在NFD启动前捕获另一个镜像模块路径差异（UnixFace位于stream_socket），两worker无NFD子进程即失败，private已清理。记录在`results/local-baseline-r5/`；下一次先要求独立应用导入PASS，再执行完整本地集成。

Local R6：两个raw正例、坏签名/错根负例及Controller真实PUBPARAMS均通过。NDNSF首边界为Controller的PIB缺少目标公开证书，无法加密permission回复；身份准备新增只含公开证书/公钥的PIB安装，并用ndnsec复读核对，TPM私钥集合必须仍只有自身一把。另设角色session.conf避免默认/etc路径；清理先TERM容器leader，让其转发信号后卸载FUSE，避免整组TERM过早终止挂载helper。完整失败与清理记录保留在`results/local-baseline-r6/`，不计服务PASS。

Local R7：NDNSF取得权限并返回正确ECHO，但探针错误要求旧V2绑定未填充的ACK认证字段为true；镜像内requestServiceSelect源码确认仅复制provider/service/request/status/payload/telemetry，字段默认值不能反推验签失败。保留真实ACK回调/选择/响应判据，新增原生错误trust-root负例提供行为证据。旧Controller run为无限循环，包装器stop无法join，改用已有C++ executable受控终止，不修改库；使用空的显式bootstrap-token文件避免其创建示例身份。原记录在`results/local-baseline-r7/`，仍为FAIL。

Local R8：raw双向/签名负例、正常NDNSF服务、合法CONTROL、未授权ECHO原生拒绝及所有进程正常受控清理均通过。新增错根用例在NAC-ABE PUBPARAMS认证时abort134；此前collector只允许权限阶段拒绝，因此保留本次FAIL。契约改为承认实际更早的认证边界，并用精确原因+退出码识别该独立负例，添加拒绝任意crash/timeout的单测；不修改镜像、不声明旧库具备优雅错误恢复。原日志见`results/local-baseline-r8/`。

从仓库根运行本地精确镜像验证（两逻辑节点，仅LOCAL_PASS）：

```bash
python3 Experiments/TigerCluster/jobs/baseline/submit.py local \
  --profile Experiments/TigerCluster/profiles/two-node.json \
  --output Experiments/TigerCluster/results --run-id local-baseline-r1 \
  --sif .local-tmp/spec180-candidate-r119/spec180-runtime.sif \
  --apptainer /usr/local/bin/apptainer
```

远端先将runtime/apps/jobs/baseline/profiles文件作为同一完整bundle传入，再用同一入口的`prepare`核对命令，`submit`提交；output必须为共享可写目录。`--workload service-echo`运行第二个复用消费者，固定输入变为reused-runtime，仍使用同一基础设施和安全负例。每次采用新run-id；已有submission记录禁止重复提交。结果为`<output>/<run-id>/run.json`，终态和每节点日志在同一目录，私有身份结束后删除。运行期间不得改动已提交bundle。
