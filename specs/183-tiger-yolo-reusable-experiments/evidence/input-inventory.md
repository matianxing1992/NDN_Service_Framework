# T001 Receiving Input And Interface Inventory

**Date**: 2026-09-06 (America/Chicago)
**Status**: INVENTORIED / execution WAITING_EXTERNAL_INPUT
**Subject**: `TigerClusterExperiments` at `1a57ad1da94272136c35ef58a664570f82df23cf` before this inventory commit.

T001完成的是接收清点，不是构建/模型/SIF/集群资格。本轮没有编译、模型下载、修改依赖仓库或提交Slurm。实际读取结果与交付声明分别列出。

## Source Reconciliation

Follow-up 2026-09-06: the three exact dependency revisions listed below have now
been fetched from their corresponding official project forks into independent
ignored bare caches under `Experiments/TigerCluster/.cache/source-git/`:
`nacAbe.git`, `ndnSvs.git`, and `ndnSd.git`. Each fetch used the locked SHA,
`--depth=1 --no-tags`; `refs/spec183/locked` retains it. All three
`git fsck --full --no-reflogs` checks returned 0 (unborn bare HEAD notice only).
Total allocated cache size was 2,875,392 bytes. The original dependency
worktrees, including dirty NDNSD, were not changed. This supersedes only the
missing-source-object finding below: sealed source archives/build inputs and
signed model package are still outstanding. The locked base SIF was transferred
read-only into the ignored local cache and verified below. SSH login was
reconfirmed as itiger/tma1; no job was submitted. Historical observations below
remain unchanged for provenance.

锁为 `Experiments/TigerCluster/development-handoff.lock.json`，运行源码不随实验分支HEAD自动变化。

| Repository | Required revision | Local HEAD observed | Disposition |
| --- | --- | --- | --- |
| NDNSF | `447f7584072142edea8c8e2ae3ed4ffdc5fbd0c5` | `1a57ad1da94272136c35ef58a664570f82df23cf` | locked commit exists；生产Core/DI/Repo/Python/examples/wscript无差异；tests仅两交付工具文件不同 |
| NAC-ABE | `5ed23e68520fb9c4ef747c51d6d18c915ff88fc4` | `c3aafa6ec5a566879942107c7b20855659c9dfb9` | clean Experimental；locked object locally missing |
| NDN-SVS | `9f2d8a47cd2a25a5f9ade661c9dbe8acd6416a20` | `b3e3814df135b3cda1bd69c605e145fb3468f204` | clean master；locked object locally missing |
| NDNSD | `375a35c5706d5a6b6f176cd98b72d63dcb7f5b10` | `25f7ad9d2f8848c10025e71358e3dafd62c348c0` | ndnsd.pc.in modified；locked object locally missing；保留原目录 |

各目录实际执行`git rev-parse HEAD`、`git status --short --branch`、`git cat-file -t <locked-revision>`（缺对象返回128）。接收须使用隔离输入目录，不能reset原NDNSD或以旧库替代新ABI。
宿主`/usr/bin/g++`9.4.0、`/usr/bin/ld`binutils2.34；pkg-config显示NDN-CXX0.9.0/SVS0.1.0/NAC0.1/NDNSD0.1.0。包版本不证明源码匹配。尚未做新candidate loaded-library检查；顺序NAC/SVS→NDNSD→Core/native/apps/两个扩展，最大`-j2`。

## Runtime Outputs

`adapters/slurm-apptainer/templates/development-runtime.def.in`的final-native-sha256清单要求九项：`libnac-abe.so`、`libndn-svs.so.0.1.0`、`libndnsd.so.0.1.0`、`libndn-service-framework.so.0.1.0`、`ndnsf/_ndnsf*.so`、`py_repoclient/_py_repoclient*.so`、`App_ServiceController`、`di-native-provider`、`di-native-fault-provider`。
扩展须为同一CPython3.10；内部依赖、SONAME、RPATH仍需T008/T011实测。模板NDNSF目标为`ndn-service-framework,libndn-service-framework.pc,App_ServiceController,di-native-provider,di-native-fault-provider`，由本地container-native builder编译，不能带宿主.so进入镜像。

## API And Workload Inventory

| Actual owner | Source-verified contract | Implementation consequence |
| --- | --- | --- |
| `examples/python/NDNSF-DistributedInference/yolo_2x2/user.py::_load_yolo_ack_driven` | 签名catalogue、offer trust、request/lifecycle ID、真实APPClient；ACK固定1500ms | 复用正常路径，拒绝bare input reference/旧HMAC/offline-oracle替代 |
| Same User | 正常路径一次`handle.response(args.timeout_ms)`，journal requestCount=1 | T005逐请求启动User，独立ID/目录；不能仅传legacy sequential参数就宣称四次执行；Provider保持运行 |
| `tools/ndnsf-di/prepare_spec180_yolo_case.py::prepare_case` | Y-B四个distinct身份、shared-backbone-two-shard-v1；要求已签名package/registry | 复用model/身份规则；双节点放置与跨机Data由T003/T005接线 |
| `Experiments/NDNSF_DI_YoloAckDriven_Minindn.py` | `--case`；input/ABI检查在网络前；缺输入返回78，未合格执行返回2 | 保留真正输入门；78不是执行PASS |
| `adapters/yolo/reference.py` + numerical contract | fixture/oracle/输入/响应hash，shape/class、atol1e-3/rtol1e-4 | 复用现有比较器，不放宽标准 |
| `runtime/baseline.py::container_command/Processes/configure_routes` | CPU v1、isolatedHOME、cwd=/bundle、受控env、进程清理/TCP路由 | T003扩展共享原语；现有调用没有GPU选项 |
| `jobs/spec180/supervise-tiger.py` | 单节点四角色、固定示例身份/readiness预算，现有terminal collector | 不能仅修改Slurm nodes；不复用固定sleep作为readiness |

### Model Inputs

已实际校验fixture `tests/fixtures/spec180/yolo26n/fixed-fixture.ppm`：SHA256 `7edf1f524ef450be6ee2304b3c0b47b70c3d72610c18f2f2fa28157d7b8a113c`。
历史记录 `specs/180-ack-driven-cross-model-qualification/evidence/t004-yolo-export-current-20260902.md`给出接收期望：checkpoint5,544,453 bytes/hash `9b09cc8bf347f0fc8a5f7657480587f25db09b34bf33b0652110fb03a8ad4fef`；graph `956ee2aa62f34c1ac035b85a837b70786bfa8da3ae8650e7539abbe572d0dd2a`；weights `1a998d3d56c0103e57ea6df557370a219a3df53380572b4e9337ff26b4a94a7f`；oracle `ed5a23d73e3cda04677bfc9d895732b44e1455f20d9dc433ae5462eb7b9b7175`。这些不是本轮实测artifact hash。
本机Tiger目录及旧历史checkout未发现canonical package/full-model-output.npy；交付所述.local-tmp/.codex-tmp包目录不存在。需要接收当前签名package，或按既有exporter/lock本地生成并由注册authority签名。旧manifest已被priority变更作废，不得复用。Exporter锁为Python3.10/Torch2.4.1/Ultralytics8.4.56/ONNX1.17.0/ORT1.19.2/NumPy1.26.4；只用于离线export，不是部署依赖。私钥内容未读取。

2026-09-07只读检查发现远端 `spec180-runtime-b6710fd6` 候选包含与历史记录一致的 YOLO26n 图、权重、oracle 和 `shared-backbone-two-shard-v1` catalogue，但它仍是 Spec180 的 SIF/profile/run record，使用旧 `/example/group`，没有 Spec183 source/runtime/dispatch seal。因此只能作为待验证模型输入候选，不能作为 Spec183 runtime 或资格证据；详见 [spec180-candidate-reuse-audit.md](spec180-candidate-reuse-audit.md)。

## Site And Capacity

- SSH实测itiger/tma1可达，squeue无本用户job；未提交新job。
- 登录Apptainer1.3.4-1.el9，本地/usr/local/bin/apptainer为1.3.4；2026-09-07
  通过一个有界的 `srun` 版本探针在 `itiger02` 实测 compute 为
  `1.5.3-1.el9`（`/usr/bin/apptainer`，RPM `apptainer-1.5.3-1.el9.x86_64`），
  与登录节点不一致。该探针未启动 YOLO、SIF 构建或模型作业；T011 前必须在本地
  安装/验证与 compute 匹配的 Apptainer，不能把登录节点 1.3.4 当作构建版本。
- bigTiger up，公布GRES rtx_6000/rtx_5000/h100_80gb；账号devs/QOS normal。不是两节点同时可分配证明。
- 远端base锁定路径存在，stat为3,525,861,376 bytes；本地
  `Experiments/TigerCluster/.cache/base-sif/spec180-runtime.sif` 已按完整内容校验为
  `sha256:b6710fd696a7f962f67f67a54278d92a15babb856c4af428a3f04ba54dc83285`，与 lock 一致。
  该文件仍是历史 Spec180 基础输入，不是 Spec183 最终 SIF，也不替代 T007/T008/T011。
- 本机`Experiments/TigerCluster/images/spec180-runtime-r119.sif`、`.local-tmp/spec180-candidate-r119/spec180-runtime.sif`均不存在。
- 本地最近检查可用约18,433,155,072 bytes；本次 compute 探针在 `itiger02`
  看到 `/tmp` 为 14T、可用约14T，`/project` 为900T、可用约838T；这只是该
  allocation 的容量观察，不能替代用户quota或完整scratch写入/`fsync`验证。
  未取得Spec183构建峰值，不批准大构建。
- 2026-09-07只读扫描 `/project/tma1/ndnsf-di` 未发现 `spec183` profile、签名
  package、candidate 或 collector 输入；仅发现旧 Spec170/Spec180 YOLO/SIF
  材料。旧候选不能直接升级为Spec183输入，仍需重新绑定 source/runtime/dispatch
  seal。
- 15分钟初始预算内startup120+4×request60+cleanup30=390秒，余510秒仍须覆盖stage/hash等实际耗时；未实测，不据此自动提交。

## Build And Test Selector Registry

以下为已核对源码的选择器，非已执行结果：

- Container：唯一`build-local-sif.sh`+development-runtime.def.in；NAC CMake `--parallel 2`，SVS/NDNSD Waf `-j2`，Core上述五targets，两个pip native builds。
- Host：T008解析fresh prefix/build实际路径，Waf `--with-tests --with-examples --disable-local-dependency-prefix --nac-abe-prefix=<fresh-prefix> --ndn-svs-source-tree=<locked-source> --ndn-svs-build-tree=<fresh-build> --boost-includes=/usr/include --boost-libs=/usr/lib/x86_64-linux-gnu`并显式使用系统toolchain。追加`unit-tests,integration-tests`目标；模板内无tests构建不能成为unit PASS。这里的路径标记不能直接执行。
- Focused Python：`python3 -m pytest -q tests/python/test_spec180_yolo_application.py tests/python/test_spec180_yolo_equivalence.py tests/python/test_spec181_y_b_grant_seam.py tests/python/test_spec180_terminal_collector.py`。
- Builder：`python3 -m pytest -q tests/python/test_build_local_sif_record.py tests/python/test_development_handoff.py Experiments/TigerCluster/tests/test_development_runtime_template.py`。
- Shared lifecycle：`python3 -m pytest -q Experiments/TigerCluster/tests/test_baseline.py Experiments/TigerCluster/tests/test_worker_lifecycle.py`。
- Native focused：新integration-tests二进制的`--run_test=Spec181ExactTensorTransport,Spec175NativeAssembly`；不替代T008/T009完整选定suite。
- MiniNDN：`Experiments/NDNSF_DI_YoloAckDriven_Minindn.py --case Y-B`；env由validate_inputs/prepared case产生，T005/T010需登记完整manifest，当前不是可直接运行的命令。
- New Spec183 focused tests按T002–T006生成；T007前只记开发回归，不记正式qualification。

## Controlling Findings And Owners

1. **BLOCK before SIF — old workload coupling (T002/T010/T011).** builder无条件调用spec175_host_gate.py、ndnsf-di-spec175-preflight和jobs/spec175/workload.json。只接受tiny-onnx/M01/seed1750001或历史42项矩阵，不能接收真实YOLO证据。保留旧schema，增加显式Spec183 dispatch和真实YOLO receipt校验，仍复用唯一builder；未知schema/错workload/source/失败或缺原始证据须在Apptainer调用前拒绝。不能仅放宽schema或伪造M01。
2. **BLOCK before multi-request qualification — one-shot User (T005/T006).** 独立request/attempt/lifecycle目录和每次数值文件；T007核对实际执行次数，warmup/measured分开，不能靠配置推断。
3. **WAITING_EXTERNAL_INPUT — physical sources/base/package (T002/T008/T011).** 隔离接收精确输入，hash/容量门后构建；模型/签名registry/private locator列缺口，不改原工作树。
4. **DEFERRED — allocated substrate (T012).** 计算节点版本/GPU/scratch/互通必须真实allocation验证，登录信息不关闭此项。

## Workflow Evidence

Context project/active health均PASS；首次query因低熵required=T001拒绝，改用完整feature basename通过。CodeGraph index up-to-date并定位baseline，再逐项核对源码。GSD为degraded/W019：spec183-handoff不是canonical顶层文件；保留旧phase35，使用独立Spec183 tasks/handoff恢复，不声称GSD完全PASS。
