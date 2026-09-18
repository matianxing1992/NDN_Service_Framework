# Native Dependency Closure

本机 NDNSF 构建只接受一套已安装的全局依赖闭包。依赖不从仓库、`/tmp`、
`.codex-tmp`、`ndnsf-build-work` 或某次实验的 install 前缀直接消费；缺失或 ABI
过旧时先安装到本机全局目录，再重新配置和构建。历史证据中的临时前缀只用于解释
当时的失败，不能作为当前构建输入。Waf 和两个 Python binding 都按这个规则
拒绝 host 对 `/opt/ndn-base`、`/opt/onnx`、`/opt/ndnsf-stage` 等容器根的误用；
只有显式 `NDNSF_CONTAINER_BUILD=1` 的容器构建才允许其声明的 SDK 根。

本机的 Boost 1.71 与 `libndn-cxx` 不是同一个依赖层。Boost 头文件和库使用系统配对：
`/usr/include` 与 `/usr/lib/x86_64-linux-gnu`。本机已安装的 NDN-CXX/NFD 使用
`/usr/local/include` 与 `/usr/local/lib`。因此，“Boost 1.71”不需要通过仓库目录
`.local-boost171` 来选择。

这里不把 Boost 强行搬到 `/usr/local`：该前缀当前提供的是已经安装的
NDN-CXX/NFD，而本机匹配的 Boost 1.71 头文件和库只存在于系统配对目录。
NDN-CXX 的安装前缀与 Boost 的安装前缀可以不同；需要保持一致的是同一构建和
运行进程使用的 NDN-CXX/NFD 真实文件、ABI 和摘要。除非另一台机器同时提供并
验证一整套兼容的 `/usr/local` Boost，否则改变 Boost 搜索目录只会引入新的
ABI 混用风险，不会解决当前的同 SONAME `libndn-cxx` 混用问题。

`.local-boost171` 这个目录名不是当前 Boost 安装的名称。经本机核对，它没有
`include/boost`，只保存历史的 `libndn-cxx`/`libndn-svs` staging；因此不能把它
当作 Boost 1.71 的替代路径，也不能把它加入 host 的 `LD_LIBRARY_PATH`。

## Current host policy

| dependency | canonical location | rule |
| --- | --- | --- |
| Boost 1.71 | `/usr/include`, `/usr/lib/x86_64-linux-gnu` | use the matching system pair |
| NDN-CXX 0.9.0 and NFD | `/usr/local/include`, `/usr/local/lib` | one real `libndn-cxx.so.0.9.0` for the host request path |
| NDN-SVS, NAC-ABE, ONNX full-protobuf, tokenizer bridge | `/usr/local/include`, `/usr/local/lib` | one installed header/library/archive pair; no checkout prefix |
| ONNX Runtime 1.26 | `/opt/onnxruntime` (versioned target `/opt/onnxruntime-1.26.0`) | host global versioned SDK; use the installed `onnxruntime.pc` closure; do not point at a model or build checkout |
| `.local-boost171` | historical checkout staging | never enter an ordinary host `LD_LIBRARY_PATH` or implicit Waf/pkg-config search |

当前两份同 SONAME 的 `libndn-cxx.so.0.9.0` 字节不同：

```text
/usr/local/lib/libndn-cxx.so.0.9.0
  sha256 cdb79d9f282b7c8528bf2660d58ab896fce6c2c14a51eb9415ec4440cfcc8531
.local-boost171/lib/libndn-cxx.so.0.9.0
  sha256 508f0fd902094740f9b5c5b654ebe85541c097293ea3f625bfdbcbb7bc3a712
```

同 SONAME 只表示动态链接器的名字相同，不证明 ABI、符号实现或 NFD 对端行为
相同。Waf 现在固定使用 `--disable-local-dependency-prefix`；传入
`--enable-local-dependency-prefix` 会直接失败，因为历史目录已退役。运行器在 host 模式还会在
启动 MiniNDN 前拒绝从该目录加载 `libndn-cxx`。

本机 DI 不再接受显式的 NAC-ABE/SVS/ONNX build prefix。`ldd`、真实路径和 SHA-256
必须同时记录；只看 `import` 或 SONAME 不足以通过门禁。容器构建可以使用容器内的
`/opt/ndn-base` 等声明 SDK 根，但那是容器自己的全局闭包，不是宿主临时路径。

配置入口现在也对此做 fail-closed 处理：默认 host Waf 在 `pkg-config` 解析前检查
`PKG_CONFIG_PATH` 和 `PKG_CONFIG_LIBDIR`，并在解析后检查
NDN-CXX、NDN-SVS、NAC-ABE、OpenSSL、NDNSD、protobuf、ONNX Runtime、GTK 和
GStreamer 的 include/library/compiler/linker 路径，若选中了仓库的
`.local-boost171`、`/tmp` 或未声明根就在 configure 阶段退出。两个 Python binding 的 `setup.py` 对
`pkg-config`、显式 NDN-SVS/NAC-ABE 前缀以及 `NDNSF_LIBRARY_DIR` 执行全局根检查；
宿主 `NDNSF_LIBRARY_DIR` 必须是唯一的 `/usr/local/lib` 安装目录，构建树、checkout、
`/tmp`、`.codex-tmp` 和 per-run install 前缀都会被拒绝。容器 APP 只有在显式
`NDNSF_CONTAINER_BUILD=1` 时，才可使用声明的 `/opt/ndnsf-stage/lib` 等镜像内根。
`NDNSF_RUNTIME_RPATH` 也不能把宿主绑定到临时依赖目录。
维护的 MiniNDN/本地实验启动器同样只设置 `/usr/local/lib`，不会继承调用者的
`LD_LIBRARY_PATH`，因此 checkout、per-run 或旧构建目录不能在运行时重新进入闭包。
容器 Python binding 构建必须显式设置 `NDNSF_CONTAINER_BUILD=1`；该标记才会把
容器 base/APP SDK 根加入依赖闭包。宿主构建即使机器上存在同名目录也不会自动
获得这个例外。容器模板的 `PKG_CONFIG_PATH` 不再包含 `ndnsf-build-work` 这类
临时目录。
安装脚本会清除外部 `PKG_CONFIG_PATH`、编译器搜索路径、linker flags 和
`LD_LIBRARY_PATH`，再以 `/usr/local` 配置每个 Waf/CMake 依赖；已安装的 OpenABE
也必须能从这个前缀解析，不能只因为 `ldconfig` 中出现同名 SONAME 就跳过核验。
`NDNSF_RUNTIME_RPATH` 只有在容器 APP 构建中才可包含受限的 `$ORIGIN/...` loader
token，并且必须显式传入 `--allow-container-runtime-rpath`；host 依赖的
pkg-config/linker flags 不接受该 token。容器仍必须同时声明 `/opt/ndn-base`
等完整 SDK 根。
这不会把容器的 `/opt/ndn-base` 或显式声明的容器 SDK 根误判为宿主 `/usr/local`；它只阻止旧的
同 SONAME staging 混入当前 host 闭包。宿主安装脚本也必须把 OpenABE、NAC-ABE、NDN-SVS
及其运行时依赖安装到全局前缀，不能把 `$DEPS_DIR/local` 当作运行时或构建前缀。
当前脚本将 `/usr/local` 作为 Waf、CMake 和 OpenABE 的显式安装前缀；
`dependencies/` 下的 checkout 只用于编译，不会被写入运行时搜索路径。
`install_ndnsf_stack.sh` 对 `libndn-cxx`、`ndnsd`、`libndn-svs` 和 `libnac-abe`
执行最低版本和安装 prefix 检查，并验证 OpenABE、ONNX full-protobuf 与 tokenizer
bridge 的实际文件都解析到 `/usr/local/lib`。缺失或过旧的 pkg-config 依赖会触发源码
重建；无法由该脚本重建的 ONNX/tokenizer 文件会直接阻止继续；
`--no-dependencies` 也不能绕过闭包检查。安装 Core/DI 后，脚本从同一 `/usr/local/lib`
计算摘要回执再构建两个 Python binding，因而不会把未安装的 checkout 产物当作依赖。
安装器不会复用未知来源的 Waf cache：默认每次用清理后的 `/usr/bin/gcc`、`/usr/bin/g++`
、`/usr/bin/ld`、`/usr/bin/ar` 及 `/usr/bin/pkg-config`，并清除 `PKGCONFIG`、
`LD/AR/AS/RANLIB/NM/STRIP` 等工具覆盖和未污染的 linker 环境重新 configure；
`--no-configure` 被拒绝。`--check-dependencies` 也只调用 canonical
`/usr/bin/pkg-config`，不会受 PATH 中的 shim 影响。

## Current host build confirmation

2026-09-17 的活动构建树为历史候选 `build-spec187-local-nac-r1`。它的 Waf cache、DI/Core
共享库、Python extension 及两次 YOLO MiniNDN 运行的 `ldd` 曾解析到上表的
`/usr/local/lib/libndn-cxx.so.0.9.0`，但该树的独立依赖前缀不再满足当前宿主闭包，不能作为
新的验收输入。仓库中仍存在的旧构建树
（例如带有 `.local-boost171` RPATH 的 ASan 树）不属于当前候选，不能通过修改
`LD_LIBRARY_PATH` 临时复用；应重新配置并让 Waf 生成新的依赖闭包。

这里的“统一”指同一进程内所有 NDN-CXX、NDN-SVS、NAC-ABE、ONNX 和 tokenizer
bridge 使用全局安装的真实文件和摘要。本机 Boost 1.71 仍固定使用系统头文件和库
目录；其余 NDNSF 直接依赖安装到 `/usr/local`。SIF 内部使用它自己的 base/APP
全局闭包，不能把宿主文件带进镜像。

## SIF boundary

SIF 内不使用宿主 `/usr/local/lib`。base SIF 将稳定 NDN-CXX/NFD 和系统 Boost 闭包
安装到 `/opt/ndn-base/lib`，NDNSF APP 的 Core/DI/SVS/NAC-ABE 库放在 APP 自己的
只读目录。容器内所有 ELF 和 Python extension 必须解析到已声明的 base/APP 路径；
宿主源码树、`.local-boost171` 和未声明的 `/usr/local/lib` 都是候选输入错误。

## Verification boundary

Host-local builds must install the matching Waf outputs before building either
Python binding. Run this from the same configured tree that produced the native
outputs; a successful compile in a checkout is not an installed dependency:

```bash
env -u PKG_CONFIG_PATH -u PKG_CONFIG_LIBDIR \
  PATH=/usr/bin:/bin:/usr/sbin:/sbin:$PATH \
  /usr/bin/sudo -n ./waf install -j4
```

The binding and later local runs then use only the installed root:

```bash
PYTHON_WRAPPER="/absolute/path/to/pythonWrapper"
for path in "$PYTHON_WRAPPER" /usr/local/lib /usr/lib/x86_64-linux-gnu; do
  test -d "$path" || { echo "missing absolute dependency directory: $path" >&2; exit 2; }
done
export NDNSF_LIBRARY_DIR=/usr/local/lib
export LD_LIBRARY_PATH="/usr/local/lib:/opt/onnxruntime/lib:/usr/lib/x86_64-linux-gnu"
export NDNSF_GLOBAL_NATIVE_DIGESTS="$(python3 -c 'import hashlib,json; names=("libndn-service-framework.so","libndnsf-distributed-inference.so"); print(json.dumps({n:hashlib.sha256(open("/usr/local/lib/"+n,"rb").read()).hexdigest() for n in names}, sort_keys=True))')"
```

Then inspect the extension, every transitive DI/SVS/NAC library, and `/usr/local/bin/nfd`:

```bash
ldd pythonWrapper/ndnsf/_ndnsf*.so
ldd /usr/local/bin/nfd
sha256sum /usr/local/lib/libndn-cxx.so.0.9.0
```

The maintained YOLO runner performs the same identity check before creating NFD. The
maintained `scripts/spec180_native_build.py` also compares freshly built Core/DI bytes
with `/usr/local/lib` and refuses to invoke Waf when the global install, Waf cache,
pkg-config directory, selected DSO realpath, or RPATH is missing, stale, or outside
the global closure; it passes `NDNSF_GLOBAL_NATIVE_DIGESTS` to both binding builds. Direct
`setup.py` or pip invocation must provide the same receipt variable. A failure is a
host dependency/preflight boundary, not a protocol result.
Historical Spec evidence may mention `.local-boost171`; those records remain unchanged
and are not current dependency policy.
