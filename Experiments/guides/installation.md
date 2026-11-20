# 安装与配置

唯一宿主安装入口是[install_ndnsf_stack.sh](../../install_ndnsf_stack.sh)，
根Waf只编译本仓库，外部项目使用各自构建系统。
当前脚本的支持平台、源码pins、SDK版本及组件范围，以该脚本和锁文件为准，
不能从旧实验的Boost/ORT版本或Python包名推断兼容性。

从仓库根先查看计划；本文不授权自动执行系统安装：

~~~bash
./install_ndnsf_stack.sh --source --plan
./install_ndnsf_stack.sh --check-dependencies
~~~

新环境确认平台和依赖授权后，可按当前help选择 --source、--deps-only、
--no-dependencies、--components di 或 --prefix。按实际RAM和编译峰值选择 --jobs；
小内存开发机先用2，避免并行编译触发swap或OOM；
已有配置的增量开发优先[复用 build/](local-development.md)，
避免旧installer临时build覆盖正确的Waf锁。

configure.sh是兼容包装，保留configure-only语义。--plan/--dry-run不等于已安装或configure通过。

## SDK closure

普通OS开发包、NDN库、ONNX full-protobuf、ONNX Runtime C++ SDK和tokenizer bridge
是不同的依赖边界。pip onnx或numpy成功不证明C++ SDK齐全。
缺receipt、错误Boost ABI或装载旧库应修实际闭包，不伪造回执/放宽比较。

同源安装后检查实际console入口、native应用及动态库解析，再按任务运行相应回归和
[installed MiniNDN](local-validation.md)。仅check-dependencies、--help或build成功不证明推理成功。
生产宿主采用全局已声明依赖，不把历史 .local-boost171 或Spec暂存路径当可选运行回退。

SIF与宿主是两套ABI边界：见[SIF构建](../TigerCluster/docs/sif-build.md)。
构建/依赖调整之外的模型与参数变化复用原镜像。
本页取代旧 unified-stack-install.md 和 configure-dependencies.md 中相互重复/过期的操作段。

## 模型与新部署输入

### 最小模型、准备命令与两种调用

默认 `unit` 与 `integration` 档只读受版本控制的 fixture，不读外部模型。需要真实模型时，用仓库内
唯一登记的配方 `Experiments/recipes/qwen3-0.6b-int4.json`（0.6B INT4，CPU 可跑）；更大的模型走
既有显式部署/实验入口，不在本机准备。

准备（两种来源，任选其一；`--asset-root` 的父目录须已存在）：

~~~bash
python3 -m venv /path/to/model-tools
/path/to/model-tools/bin/python -m pip install --only-binary=:all: \
  -r Experiments/recipes/model-tools-py38.txt

# 显式下载（经授权）
/path/to/model-tools/bin/python Experiments/prepare-model-assets.py \
  --recipe qwen3-0.6b-int4 --asset-root /persistent/models/qwen3-0.6b-int4-v1 --allow-download

# 本地源镜像（离线）
/path/to/model-tools/bin/python Experiments/prepare-model-assets.py \
  --recipe qwen3-0.6b-int4 --asset-root /persistent/models/qwen3-0.6b-int4-v1 \
  --source-dir /persistent/models/qwen3-0.6b-int4-source --offline
~~~

依赖：Python 3.8 工具环境、配方锁定的上游 revision、至少 4 GiB 空闲；准备过程保留约 1.8 GiB
不可变输入输出。`--offline --allow-download` 会被拒绝；缺文件、LFS 指针、错误 revision、
权重变化、空间不足或转换失败都不会产出可准入的 SEALED 清单。

准备成功后，清单路径是 `<asset-root>/asset-manifest.json`。两种调用都只把该清单交给 runner，
由它按配方与逐文件承诺校验，再把精确的角色路径传给原生用例：

~~~bash
# 源码树
python3 tests/run-checks.py --profile model \
  --asset-manifest /persistent/models/qwen3-0.6b-int4-v1/asset-manifest.json

# 已安装前缀（从源码树外）
<prefix>/bin/ndnsf-checks --profile model \
  --asset-manifest /persistent/models/qwen3-0.6b-int4-v1/asset-manifest.json
~~~

`--profile all` 会**主动包含** `model` 与 `qualification` 档，因此它要求这些档的输入都已准备；
只跑默认检查时用 `--profile unit` 或 `--profile integration`。清单无效或缺失按前置失败处理，
不计为通过；准备回执只证明文件与结构契约，不代表原生执行或数值资格。

普通安装只安装软件，不隐式下载模型。先按[固定模型配方](../recipes/README.md)
显式准备SEALED资产；随后在已经安装并具有完整receipt的prefix上单独运行：

~~~bash
./install_ndnsf_stack.sh --prepare-experiment qwen06b-tp-cpu --prefix /usr/local \
  --asset-manifest /persistent/models/qwen3-0.6b-int4-v1/asset-manifest.json \
  --work-root /persistent/experiments/qwen06b-tp-cpu --namespace /my-lab/cpu --offline --plan
~~~

确认路径后去掉--plan实际准备。资产root和实验root的父目录须已存在；实验root必须尚未存在。
该动作不与--source/--native-only/卸载等安装模式组合，也不启动推理。它通过所选prefix中
的原生工具、ndnsec和OpenSSL生成新凭据及配置，不复制旧results、PIB或baseline。
在准备者实际使用的Ops解释器中检查MiniNDN/Mininet，缺平台依赖时先补软件安装。
MiniNDN要求root：准备和运行须使用同一owner与可被该owner信任的父目录，
例如root所有的/var/lib/ndnsf-experiments。不要把用户现有凭据改属root。
实验包含持久化Conversation journal，拒绝/tmp、/run、/dev/shm；模型缓存也不放临时目录。
准备receipt的CONFIGURATION_AND_NATIVE_INPUTS_CHECKED不代表runtime准入或推理成功。
