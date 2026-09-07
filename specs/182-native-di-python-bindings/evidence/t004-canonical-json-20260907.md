# T004 Typed Canonical JSON Foundation

## Status

2026-09-07 / PARTIAL。为生产 wire/core serializer 增加内部 typed JSON helper；当前尚未
替换 NativePlanSealer 旧 encoder，不关闭 T004，也不把 helper 的测试当作完整 Selection 验收。

## Dependency and Design

引入原样 nlohmann/json v3.11.3 single header，源与 MIT license 哈希在
NDNSF-DistributedInference/cpp/vendor/nlohmann/README.md 冻结。只用于原生实现，
不增加共享运行库、Python 运行时或构建时联网步骤。复用 parsing/type/escaping，
而不是使用丢失 JSON scalar/empty-container 类型的 PropertyTree 生成 canonical 字节。

nativeCanonicalJson 实现 sdk/placement.py::canonical_bytes 的排序、无空白、ASCII escape
及 allow_nan=False 约定。浮点格式用 classic locale 下最短回转精度和 Python 指数显示边界。
离线 author-canonical-json-vectors.py 以 Python stdlib json.dumps 冻结独立 oracle，
含整数上下界、空数组/对象、Unicode scalar 排序、负零、subnormal、最大 double 和固定种子随机值。
fixture SHA-256：419df7743abfb43ff14268e6b441e2fc5b70b1e7fdbec404520a8783080de8d9。

## Attempt r1

原始 `.codex-tmp/spec182-t004-canonical-json-r1/`：-j4 build exit 0（13.11s）。
focused.log exit 201：2027/2028 assertions PASS；DBL_MAX 错输出 2e+308。
首边界为浮点 helper 的 round-trip 判断：std::istream 在溢出时设置 failbit 并留下
最大有限值，比较时未检查 stream 状态。修复为检查解析成功后才比较，不改 oracle。
后续 r2 使用独立目录保存重试证据。

## Attempt r2 / Focused PASS

`.codex-tmp/spec182-t004-canonical-json-r2/build.log`：系统工具链，同一已验证 ABI build 目录，
`waf ... build --targets=unit-tests -j4 -v` exit 0。focused.log：
`build/unit-tests --run_test=Spec182CanonicalJson --report_level=detailed --log_level=message`，
2/2 cases、2028/2028 assertions PASS。未执行网络/集成/全量回归。
内部 helper 从 Waf 安装头文件通配集合排除，第三方类型不扩散到公开 SDK。
原样库文件与 license 可随源码交付，运行构建均不需要在线下载。
设计 validator 与 git diff --check PASS。下一步用该 helper 构造完整 role/assembly/
dataflow/device/grant/generation JSON，修复 tensor shape 类型后再替换生产 sealer。
