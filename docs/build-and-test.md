# Build And Test

本开发机（6 逻辑 CPU / 12 GB RAM，2026-09-08 已再次确认）原生构建默认 `-j4`；执行范围、资源观测和降档规则见
[Native Build Parallelism](native-build-parallelism.md)。其他主机单独核对资源，历史运行命令保持原记录。

原生依赖和运行时路径按 [Native Dependency Closure](native-dependency-closure.md)
执行：本机使用系统 Boost 1.71，以及 `/usr/local` 中统一安装的 NDN-CXX/NFD、
NDN-SVS、NAC-ABE、ONNX full-protobuf 和 tokenizer bridge；仓库、`/tmp` 与
`.local-boost171` 都不能作为 host 库输入。
`install_ndnsf_stack.sh` 会把 Waf/CMake/OpenABE 依赖明确配置为
`/usr/local`，并在配置前检查包版本、pkg-config prefix 和实际库文件；缺失、过旧
或解析到 checkout/临时目录时会先停止，默认流程会从 `dependencies/` 的源码 checkout
重建并安装到 `/usr/local`。`dependencies/` 只保存可重建的源码 checkout，不得出现在
`LD_LIBRARY_PATH`、RPATH 或 pkg-config 输入中。`--no-dependencies` 只禁止脚本抓取和
编译依赖源码，仍会因全局闭包不完整而退出，不能绕过这条规则。
如果环境变量自带旧的 `PKG_CONFIG_PATH`，Waf 和两个 binding 会在发现其实际路径后
拒绝 `.local-boost171`；应清理环境或显式指定完整、同一 ABI 的隔离前缀。

## Install The Stack

Recommended full-stack install:

```bash
sudo ./install_ndnsf_stack.sh
```

Useful variants:

```bash
sudo ./install_ndnsf_stack.sh --with-minindn-deps
sudo ./install_ndnsf_stack.sh --with-system-tests-deps
./install_ndnsf_stack.sh --check-dependencies
```

`--no-dependencies` 可用于已知全局依赖已经满足时跳过源码重建；它仍会执行闭包
检查。`--no-system-install` 只适合明确的 C++ build-only 诊断，脚本会在 Python
binding 阶段停止，不会把 checkout 产物当成完整安装。安装器每次都会用清理后的
系统工具链重新执行 Waf configure；`--no-configure` 会被拒绝，避免复用未知来源
的旧 cache。

## Build From Source

```bash
BUILD="$PWD/build-current"
test -d "$BUILD" || mkdir -p "$BUILD"
env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR \
  -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
  -u LD_LIBRARY_PATH -u LIBRARY_PATH -u CPATH \
  -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH -u LDSHARED -u WAFDIR \
  -u PKGCONFIG -u LD -u AR -u AS -u RANLIB -u NM -u STRIP \
  -u OBJCOPY -u OBJDUMP -u READELF \
  PATH="/usr/bin:/bin:/usr/sbin:/sbin" PKGCONFIG=/usr/bin/pkg-config \
  CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld \
  AR=/usr/bin/ar AS=/usr/bin/as RANLIB=/usr/bin/ranlib \
  NM=/usr/bin/nm STRIP=/usr/bin/strip \
  ./waf -o "$BUILD" configure \
  --nac-abe-prefix=/usr/local --onnx-prefix=/usr/local \
  --with-examples --with-tests
env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR \
  -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
  -u LD_LIBRARY_PATH -u LIBRARY_PATH -u CPATH \
  -u C_INCLUDE_PATH -u CPLUS_INCLUDE_PATH -u LDSHARED -u WAFDIR \
  -u PKGCONFIG -u LD -u AR -u AS -u RANLIB -u NM -u STRIP \
  -u OBJCOPY -u OBJDUMP -u READELF \
  PATH="/usr/bin:/bin:/usr/sbin:/sbin" PKGCONFIG=/usr/bin/pkg-config \
  CC=/usr/bin/gcc CXX=/usr/bin/g++ LD=/usr/bin/ld \
  AR=/usr/bin/ar AS=/usr/bin/as RANLIB=/usr/bin/ranlib \
  NM=/usr/bin/nm STRIP=/usr/bin/strip \
  ./waf -o "$BUILD" build -j4
```

安装一个已经配置并验证过的构建树时，必须从该树目录执行 Waf；不要在仓库根目录
再次传 `-o`，因为 Waf 会按最近的锁定配置选择输出树，可能误启动另一棵全量编译：

```bash
(cd "$BUILD" && sudo -n ../waf install)
sudo -n ldconfig
```

安装前先确认 `$BUILD/.lock-waf_linux_build` 的 `out_dir`、Boost 配对和全局依赖根
与本批证据一致。若只需补装生成的 pkg-config 元数据，可在同一目录使用
`--targets=ndnsf-distributed-inference.pc,libndn-service-framework.pc`，并设置
`NDNSF_SKIP_DEV_PIP_INSTALL=1` 避免触发无关的 editable 安装。

如果发现依赖缺失或 ABI 过旧，先用维护的安装脚本把依赖安装到全局目录，再重新
配置；不得通过 `--*-prefix` 临时指向 checkout 或 `.codex-tmp` 绕过闭包检查。

在复用已配置的构建树前，先确认它没有继承历史 `.local-boost171` 选择：

```bash
test -n "${BUILD:-}" && test -d "$BUILD"
rg -n 'local-boost171|LIBPATH_NDN_CXX|LINKFLAGS' "$BUILD/c4che/_cache.py"
ldd "$BUILD/libndn-service-framework.so" | rg 'libndn-cxx|local-boost171|/usr/local'
```

本机当前 host 候选必须显示 `/usr/local/lib/libndn-cxx.so.0.9.0`；若 cache 或
`ldd` 仍出现 `.local-boost171`，停止复用该树，重新配置并记录新的真实路径和
SHA-256。不要用临时 `LD_LIBRARY_PATH` 覆盖两个同 SONAME 文件的差异。

Build selected native DI targets:

```bash
./waf -o "$BUILD" build --targets=di-native-provider,di-native-plan-schema-smoke,di-native-plan-manifest-smoke,di-native-provider-session-smoke,unit-tests -j4
```

When a batch changes only files under `NDNSF-DistributedInference/`, reuse the
verified build tree and select the affected DI target(s) plus their focused
tests. Do not rebuild Core, Repo, or UAV targets, clear the tree, or create a
fresh whole-tree build just because the batch identifier changed. Rebuild
transitive consumers when shared headers, generated inputs, build configuration,
ABI, or dependencies changed; record that expanded boundary in the Spec evidence.

The `integration-tests` target takes its DI core and adapter sources from the
same recursive source closure as the installable DI library. Keep this closure
automatic; do not reintroduce a hand-maintained subset when adding a native TU.
For an integration batch, use the existing tree and select the target explicitly:

```bash
./waf -o "$BUILD" build --targets=integration-tests -j4
```

Python packages for source-tree development:

```bash
export NDNSF_LIBRARY_DIR=/usr/local/lib
export NDNSF_GLOBAL_NATIVE_DIGESTS="$(python3 -c 'import hashlib,json; names=("libndn-service-framework.so","libndnsf-distributed-inference.so"); print(json.dumps({n:hashlib.sha256(open("/usr/local/lib/"+n,"rb").read()).hexdigest() for n in names}, sort_keys=True))')"
python3 -m pip install -e ./pythonWrapper
python3 -m pip install -e ./NDNSF-DistributedRepo/pythonWrapper
python3 -m pip install -e ./NDNSF-DistributedInference

两个 C++ binding 的 `setup.py` 只接受已经安装的 `/usr/local/lib` Core/DI
库，并要求 `NDNSF_GLOBAL_NATIVE_DIGESTS` 与该安装逐字节一致；上面的命令仅适用于
已完成 `./waf install` 且当前 shell 已设置该摘要回执的环境。直接从 checkout
构建 `.so` 或通过 `NDNSF_LIBRARY_DIR` 指向构建树都会被拒绝。
```

## Regression Tests

Full unit test suite:

```bash
build/unit-tests
```

Focused native DI checks:

```bash
build/unit-tests --run_test=NativeArtifactMaterializerRejectsHashMismatch,NativeProviderReadinessAckControlsSelectionEligibility,NativeProviderHandlerExtractsOnlyFinalRoleResponse,NativeExecutionPlanGeneratedJsonDrivesProviderSessionSkeleton
```

Core security and service regressions:

```bash
examples/run_security_regressions.sh
examples/run_hello_auth_regression.sh
examples/run_hello_ack_payload_regression.sh
examples/run_selective_ack_custom_selection_regression.sh
examples/run_nac_abe_attribute_routing_regression.sh
examples/run_token_handshake_negative_regression.sh
examples/run_token_certificate_bootstrap_regression.sh
```

Documentation regressions:

```bash
python3 Experiments/NDNSF_UAV_Documentation_Regression.py
python3 Experiments/NDNSF_Transfer_Boundary_Documentation_Regression.py
```

The transfer-boundary regression guards the framework-wide rule that
continuous streams use `StreamInfo`/`StreamChunk`, while files, recordings,
model artifacts, manifests, and DI tensor bundles use exact-name large-data or
repo retrieval.

## MiniNDN Checks

Short script health suite:

```bash
python3 Experiments/NDNSF_Run_Minindn_Quick_Checks.py
```

Native DI real MiniNDN wiring evidence:

```bash
python3 Experiments/NDNSF_DI_NativeTracer_Minindn.py --quick-smoke
python3 Experiments/NDNSF_DI_NativeTracer_Minindn.py \
  --local-execution-only \
  --out /tmp/ndnsf-di-execution-bridge-local
sudo -n python3 Experiments/NDNSF_DI_NativeTracer_Minindn.py \
  --out results/native_di_real_minindn/default \
  --assignment default
sudo -n python3 Experiments/NDNSF_DI_NativeTracer_Minindn.py \
  --out results/native_di_real_minindn/alternate \
  --assignment alternate
sudo -n python3 Experiments/NDNSF_DI_NativeTracer_Minindn.py \
  --full-network \
  --out results/native_di_full_network/default \
  --assignment default
```

The local execution baseline runs the generated native plan through the C++
runtime and writes `local-execution-timing.csv`. The sudo runs add real
MiniNDN placement and provider wiring. The `--full-network` run submits
`/Inference/NativeTracer`, runs providers in `--serve`, and drives dependencies
through NDNSF large-data exchange. It now uses
`runnerMode=qwen-onnx-native`, which means provider roles load tiny
Qwen-derived ONNX artifacts through the C++ ONNX Runtime backend.

Regenerate the NativeTracer Qwen ONNX artifacts if the local files are missing:

```bash
python3 examples/python/NDNSF-DistributedInference/native_di_tracer/generate_qwen_native_tracer_artifacts.py
```

The generator defaults to the cached `Qwen/Qwen2.5-0.5B-Instruct` checkpoint and
does not download unless `--allow-download` is provided.

## Development Rule

Use MiniNDN for final network/security/performance validation unless an
experiment explicitly requires real hardware. Host NFD is acceptable for short
diagnosis, but clean it up and report that it was not the final validation path.
