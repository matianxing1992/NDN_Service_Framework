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
