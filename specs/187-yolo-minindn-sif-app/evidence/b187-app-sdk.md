# B187-APP-SDK

## Scope and stable exit

2026-09-15；T001 前置，base `56964ce9`。仅适配 `development-runtime.def.in` 对新稳定 base 的 SDK 路径，出口为模板静态门、相关打包测试和同一 base 内的 C++ SDK loader 正反例。完整 APP 和 YOLO/MiniNDN 仍为 PARTIAL。

## Five coverage lanes

- implementation：OpenABE/RELIC 从 `/opt/ndn-base/sdk/lib` 显式复制到 APP stage，再构建 NAC-ABE；SDK 不进入 runtime loader path。
- callers：保留 handoff → local build → APP materialization，stage/base 的 pkg-config、link 和 loader 路径显式传递。
- tests：template regression、handoff、APP packager；原生 loader oracle 为 C++，Python 不声明协议 PASS。
- build/migration：使用容器现有 CPython 3.10 的 `Python.h`；删除不匹配 focal 的 `python3.10-dev` 请求；NAC-ABE/NDNSD 使用 APP-relative/base RPATH。
- evidence：本记录唯一归档；原始日志和 immutable snapshot 在 `.codex-tmp/app-sdk-20260915/`，不入 Git。

## Static review

官方 `review-agent` 对 `review-r1/changes.diff` 及两个完整文件返回 `STATIC_PASS / B187-APP-SDK_COMPOSITION_PASS`，没有控制性缺陷。后续 C++ probe 审查要求检查 `/proc/self/maps` 可读及三个库分别被观测；已加固并复审 `STATIC_PASS`。工作区模板及测试与冻结快照相同。

## Verification

- `python3 -m pytest -q Experiments/TigerCluster/tests/test_development_runtime_template.py tests/python/test_development_handoff.py Experiments/TigerCluster/tests/test_sif_app.py`：**34 passed, 1 skipped**，6.08 秒；`packaging-tests.log`。
- base SHA-256 为 `7b4b501033f2db876ccf5c19a9637a58b8b232cf4d555f5b8a225638e180837c`。`apptainer exec --cleanenv --containall` 内使用 `/usr/bin/g++ -B/usr/bin -std=c++17 ... -ldl` 编译 C++ probe；三库从 SDK 复制到隔离 `/app/lib`，`dlopen(RTLD_NOW)` 并逐库检查实际 mappings，输出 **APP_SDK_LOADER_PASS**；`sdk-probe-positive.log`。
- 独立缺库 APP 保留 OpenABE、RELIC-EC 而省略 `librelic.so`；相同 base/loader 环境返回 **1** 且包含 `librelic.so: cannot open shared object file`；`sdk-probe-negative.log`。未从 base SDK 或 legacy current 回退。
- 以上仅证明 SDK materialization/loader；没有运行 NAC-ABE 全量编译、APP 构建、YOLO 或 MiniNDN，也没有上传。

## Remaining closure

源码预检确认当前 `wscript` 强制要求 `--onnx-prefix` 指向官方 ONNX 1.17 full protobuf 库，以及固定 Rust/Cargo 离线输入。旧 APP 模板和 handoff lock 尚未包含这两项；下一批必须封存其源码/工具链并在容器内构建，不能复制宿主 ONNX/tokenizer 静态库。已有本地源码与缓存可复用，不应要求用户提供。

还需生成当前源码的 host-gate 证据、纳入 C++ MiniNDN selector 和 native config/input，然后执行同一 pair 的两次本机验收。历史 host manifest 不自动继承到当前候选。

## Miss retrospective

- static：SDK 路径、CPython headers 和 NDNSD RPATH 在编译前修正；probe 的 maps 可读性/seen oracle 经复审补齐。
- compile-link：C++ loader probe 编译通过；完整 APP 未编译。
- runtime-test：SDK 正反例符合预期，34 个脚本测试通过、1 个跳过。
- unobserved：ONNX/Rust 容器闭包、完整 APP、host gate、YOLO/MiniNDN、Tiger。不得以本记录将 T001/T003 勾为完成。
