# 本地 native / MiniNDN 验证

本机与Tiger消费同一 installed Controller、ArtifactAuthority、Repo、Provider、Requester
和 deployment schema；Python仅编排。Tiger规则见[portable guide](../TigerCluster/docs/portable-deployment.md)。

## 维护入口

~~~bash
ndnsf-di cluster check --config deployment.json
sudo ndnsf-di cluster up --config deployment.json --timeout <seconds>
sudo ndnsf-di cluster status --config deployment.json
sudo ndnsf-di cluster down --config deployment.json
~~~

MiniNDN使用 platform.kind=minindn，声明绝对topology路径与其SHA256，
nodes覆盖全部拓扑主机。每个managed NFD使用独立socket，各actor使用独立凭据HOME。
本机/MiniNDN up的当前最大timeout为3600秒；状态与失败仍以原生结果为准。

维护的 Qwen 实验入口是 Experiments/NDNSF_DI_Qwen06B_Native_Minindn.py；
run --deployment deployment.json --verify 委托上述owner和C++ oracle。
先读当前 --help/schema，再使用明确支持的参数。
历史 --profile/stage/native-tracer 入口属于兼容实验，不证明统一部署路径。

Qwen CPU TP 三轮验收还要运行同一安装前缀中的独立未切分模型对照。使用本次部署
的状态文件，其路径为 deployment.json 中的 `storage.state_root` 加
`deployment_id/state.json`。Requester 正常退出后，确认所有角色和 MiniNDN
网络均已关闭，再用同一部署用户运行以下命令。

~~~bash
sudo /path/to/prefix/bin/multiturn-oracle \
  --deployment-state /path/to/state.json --require-multi-token --model-reference
~~~

检查每轮的实际输出数、检查点认证和提交前缀。该对照逐 token 比较独立 CPU 输出，
并报告 `providerStateValuesCompared`，据此判断是否做了 Provider 内部状态数值比较。

用 `--with-tests` 安装 DI 后，从源码树外运行同批次的 ONNX 安装探针：

~~~bash
env -u LD_LIBRARY_PATH -u LD_PRELOAD \
  /path/to/prefix/libexec/ndnsf-tests/onnx-source \
  --run_test=Spec233OnnxSource/InstalledSourceProbeUsesTheCurrentFloatingContractAbi
~~~

它启动同目录的 `automatic-source-probe`，验证实际安装库返回的 ONNX 合约版本、
浮点事实和摘要。

宿主的 MiniNDN 环境随[普通 DI 安装](installation.md#minindn-宿主运行环境)交付。
YOLO 的模型和专用入口由 `--with-yolo-minindn` 安装。可在任意目录
运行 `ndnsf-yolo-minindn --preflight-only`，检查随安装交付的脚本、Python 路径、
原生库和固定模型 SHA-256。实际两阶段实验用 `ndnsf-yolo-minindn` 启动，
每次在磁盘型 scratch 中创建独立运行目录。

授权 MiniNDN 实验用 `Experiments/run_authorization_evaluation.py --mode minindn`，
显式传入 `--ndn-svs-source`、`--output` 和所选构建的 `--unit-tests`。
它记录三个原生进程实际解析出的同一个 NDN-SVS 库。

Wi-Fi gRPC 比较在运行实验的同一个 Python 环境中加载服务器、客户端与仓库内的
protobuf 生成文件。Python 3.8 可用 `python -m pip install -r
Experiments/gRPC/requirements-py38.txt` 准备依赖，再用该解释器运行 launcher。
只有重建 gRPC 上游源码时，才需要初始化 `Experiments/gRPC/grpc` 固定 gitlink
`b8a04acbbf18fd1c805e5d53d62ed9fa4721a4d1` 及其子模块。
独立 Repo 兼容性探针 `spec192-repo-client` 要用
`NDNSF_NDN_CXX_SOURCE_TREE` 指定 ndn-cxx 源树，固定提交
`1204b7528986bea82651f3fa9088c165667cd598`。对应的候选凭据构建器
`tests/minindn/build_candidate.py` 接收 `--ndn-cxx-source` 和
`--output-dir`，在 checkout 外的新目录构建并安装候选库及 Python 绑定。
使用同一个装有 setuptools、pybind11、PyYAML、python-ndn、cryptography 和
MiniNDN 的 Python 解释器构建与验收。绑定构建禁用用户 site-packages。
默认使用安装器锁定的 NDN-SVS。需要指定其它 SDK 时，
`--svs-source` 和 `--svs-build` 必须同时提供，并符合 Waf 的 SDK 路径要求。
用 `run_spec192_real_cpp_repo.py --build-receipt` 选择该目录中的构建凭据。
加上 `--campaign-case` 和 `--preflight-only` 可以验证凭据及运行库而无需启动 MiniNDN。

Wi-Fi 逻辑测试可以单独用 MiniNDN 的基础 Python 包运行。执行 Wi-Fi 拓扑还需
Mininet-WiFi 的 `mn_wifi`、wmediumd 及实验权限。离线 Qwen workload 校验使用
Python 3.10 加 `tokenizers==0.21.4` 和 Jinja2；Ubuntu 20.04 的 Python 3.8
没有该 tokenizer 版本的预编译 wheel。当前 ONNX CPU 原生路径使用安装器交付的
原生 tokenizer 桥接库。

Go SVS 对照实验从 `Experiments/GoExperiment_AutoConfig.py --preflight-only`
核对拓扑及 tracked Linux x86_64 版 `syncps` 的 SHA-256。需要在其它 ABI 下
构建时，用 Go 1.23 在 `Experiments/` 执行
`go build -mod=readonly -o /absolute/scratch/syncps syncps.go`，通过
`--syncps` 和 `--syncps-sha256` 指定产物。模块依赖由 `go.mod` 和
`go.sum` 固定。实际运行需 MiniNDN、NFD、NLSR 和独立输出目录。

## 最短有意义的检查

- 固定source/model/tokenizer/config、实际安装程序/库以及当前图的quantization和IO dtype。
- 先沿配置→Catalog→Selection→Handler→worker→runner→结果走源码，检查生产接缝。
- 共享机制修改先做受影响native C++回归，再做小模型installed cold/warm。
  内部helper、同进程Dummy Face、手填plan/inputs不替代实际两个Provider进程。
- 调试可先用2个token覆盖反馈/decode；正式输出长度及warm条件服从任务要求。
  新准备的qwen06b-tp-cpu验收固定24/24/24个新输入、每轮32输出、无自动BOS，
  committed prefix为56/112/168。旧25/24诊断数据保留原身份，不混作这套验收输入。
- 原生独立参考核对结果和必要状态；自然退出、错误边界及角色清理单独验收。
  Repo改动先以生产读API写入、重新打开实例、按名读回。
- TP存在steps时，生产投影round-trip必须保留每条step/slot及唯一scope。
  提前发Interest不等于阻塞；确认谁真正等待future。缺绑定应前置拒绝。

## 记录和关闭

每个请求保留request/attempt、role、epoch和候选身份。追加统计只读取本次run，
模型量化标签与真实MatMulNBits/MatMulInteger及activation类型要一致。
将可重算统计归档到当前任务证据位置，关闭后回收自己的临时副本，保留模型和有效缓存。
本地通过只证明相应本地条件，不自动成为Tiger、CUDA、跨物理节点或大模型资格。

本页收敛了旧 docs/ndnsf-di-minindn-environment.md 与
docs/ndnsf-di-qwen06b-local-experiments.md 的现行操作；旧Spec184构建目录和profile示例不再是默认。
