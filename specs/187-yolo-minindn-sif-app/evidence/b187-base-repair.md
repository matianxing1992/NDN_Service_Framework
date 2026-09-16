# B187-BASE: Stable base repair

## Scope and design binding

用户要求修复取回的 base 构建脚本，包括另一台机器报告的 NumPy 私有库缺失，并实际构建与本机简单运行。参考 `origin/TigerClusterExperiments` 的 `build-base-libraries.sh`；按当前 `sif-app-delivery.md` 的稳定 base / 外置 APP 边界调整。Core/DI/SVS/NDNSD/NAC-ABE 及绑定归 APP，不能为了旧脚本的 `--runtime-libraries-only` 修改当前生产 API 或塞入旧 DI 库。

单任务批次 B187-BASE（T001 的基础制品前置），稳定出口为：锁定父 SIF + wheel 生成新 SIF，镜像内 C++ NDN-CXX/ORT SDK consumer 编译运行、NFD version、ELF closure、NumPy 三私有库哈希与 BLAS 矩阵运算通过。它仅为 `BASE_SMOKE_ONLY`，不完成 T001 pair 验收、T003 MiniNDN 或 Tiger 资格。

## Five coverage lanes

- implementation: `build-base-libraries.sh`、`base-runtime.py`，容器内修复 NumPy、清理旧应用和安装稳定 NDN SDK 路径。
- callers/lifecycle: `build-base-sif.py`、`library-runtime.def.in`，锁定输入、全新输出、绝对 temp/cache、失败记录和最终镜像验证。
- tests/fixtures: `test_base_runtime.py` 覆盖缺库、变更库、额外库和 symlink；镜像内编译 C++ `base-smoke.cpp`。
- build/migration: `base-runtime.lock.json` 绑定父镜像与 NumPy wheel；没有修改 Core/DI 实现或旧任务验收状态。
- evidence/operations: 本记录为唯一批次证据；原始数据 `.codex-tmp/base-repair-20260915/`、镜像 `Experiments/TigerCluster/images/base-repair-20260915/`，均不入 Git。

## Preparation

- Apptainer `/usr/bin/apptainer`：1.5.3，SHA256 `2cbfdcbc53a0a1eb56a1327cc42c4cfbdb48beaa03b9a26547df9e4556d3b673`。
- 找到父镜像：通过现有 `itiger` SSH 配置只读查询 `/project/tma1/ndnsf-di/candidates/spec180-runtime-b6710fd6/spec180-runtime.sif`，大小 3525861376 bytes。旧 `tigercluster` 别名失败不等于镜像不存在。
- 用户授权清理旧可重建目录后，仅移除 `build-spec185-b3-asan-ubsan-fast` 下 568 个 `.o`，共 9860990312 bytes；保留二进制和证据。逐文件清单在原始目录 `cleanup.json`。
- 旧 Apptainer preflight 首边界为相对 `APPTAINER_TMPDIR` 的 mount lookup 错误（`.codex-tmp/base-sif-preflight-20260915/build.log`）；新入口使用绝对路径。
- Context Mode project health 通过，active-Spec health 因 spec/tasks 索引过期失败，使用磁盘文档作权威；没有清空索引。

## Static review

r1 不可变快照：`.codex-tmp/base-repair-20260915/review-r1/`，仅六个新增/取回文件，批次 base `abbb02dbde1d6b977c39cd692340344e9711683b`。官方 `review-agent` 逐文件及组合审查返回 `STATIC_FAIL`：

1. P1：OpenABE/RELIC 留在默认库路径，APP 缺库可能从 base 回退；修复为隔离到 `/opt/ndn-base/sdk/lib`，不进入默认 loader 路径，最终 verifier 拒绝默认目录中的 APP 库。
2. P1：`ldd` 仅检查 unresolved，未核对来源；修复为解析 realpath，只允许系统、稳定 base 和 ORT。
3. P2：旧应用入口只清理 legacy bin；扩展到 legacy、base 和 `/usr/local/bin`。
4. P2：Python 残留模块、extension、pth/egg-link 未覆盖；清理顶层 APP 文件，拒绝额外启动钩子及 editable 引用，并记录父镜像中两个合法 hook 的摘要。
5. P2：工具和父镜像构建前后身份不够明确；补充工具 hash/version 与父文件 device/inode 的末尾复核。失败目录保留且写 FAIL，这是仓库要求，不做自动删除。

r2 快照 `.codex-tmp/base-repair-20260915/review-r2/` 返回 `STATIC_FAIL`：宿主 Python 3.8 不支持 `Path.is_relative_to`、缺少 `/lib64`/`/usr/lib64` 系统 loader 根、Python hooks 缺少 lock 中预期摘要。三项已修复并补充负例，r3 快照已提交复审。父镜像完整下载后的 SHA-256 已核对为锁定的 `b6710fd696a7f962f67f67a54278d92a15babb856c4af428a3f04ba54dc83285`；从该已验证镜像只提取两个 hook 文件计算预期摘要。

C++ compiler/linker 版本和完整命令也已纳入候选内记录。私有 SDK 仅用于后续 APP 的显式构建输入；旧 APP recipe 中的 OpenABE 路径须适配新 SDK 后才能做 APP 验收，本批不声称 APP 兼容通过。r2 的 SDK 迁移提示作为该范围限制记录，不扩展本次 base smoke 批次。

## Verification and closure

r3 官方审查返回 `STATIC_PASS` / `B187-BASE_COMPOSITION_PASS`，五 lane 无剩余控制性缺陷；运行前逐文件核对六个源文件与 r3 快照完全一致。

定向检查：`bash -n`、`shellcheck`、`git diff --check` 通过；`python3 -m pytest -q Experiments/TigerCluster/tests/test_base_runtime.py` 为 **15 passed in 0.18s**。

实际构建 r1 输出目录 `Experiments/TigerCluster/images/base-repair-20260915/run-r1/`，driver 原始输出 `.codex-tmp/base-repair-20260915/driver-r1.log`。父镜像启动、展开和 NumPy 重装通过，首次失败为 `/usr/bin/g++` 缺失，未进入 C++ 编译；build record 为 FAIL。修复为容器内安装 g++、pkg-config、Boost/OpenSSL 开发包并记录实际包版本，空间门按实际展开情况提高到 16 GiB；待复审后用新目录重试。

展开期间按同一清理授权另外移除 `build-spec185-b0c-normal` 下 597 个 `.o`（3933209984 bytes）；二进制和日志保留，清单 `cleanup-normal.json`。尚未取得 runtime PASS，保持 `PARTIAL`。

r4 官方只读复审与组合审查均 `PASS`，确认 wheel 校验后才进行容器内 APT 安装、绝对 `/usr/bin` 工具路径和编译/包版本记录。六文件快照再次与执行源码逐字节一致；定向回归 **15 passed in 0.23s**。第二次实际构建输出 `run-r2/`，driver 记录 `driver-r2.log`。

最终 **BASE_SMOKE_ONLY PASS**：Apptainer 构建成功，`%post`、`%test` 和随后直接启动最终 SIF 的验证均通过。C++ 程序在镜像内编译并输出 `/base/smoke ORT 1.20.0`；NFD 为 24.07；NumPy 为 1.26.4，三私有库摘要、BLAS 矩阵乘法、ELF realpath 来源及旧应用残留检查通过。

- SIF：`Experiments/TigerCluster/images/base-repair-20260915/run-r2/base.sif`，3759759360 bytes。
- SHA-256：`7b4b501033f2db876ccf5c19a9637a58b8b232cf4d555f5b8a225638e180837c`。
- 原始 `build-record.json`、`build.log`、`smoke.json` 在同一运行目录；精简持久记录见 [base-smoke-20260915.json](base-smoke-20260915.json)。镜像和原始日志不入 Git。
- 真实负例：在最终镜像的临时 writable overlay 中删除 OpenBLAS，verifier 以 rc=1 / `NUMPY_INSTALLED_LIBRARY_SET` 拒绝；原始 `.codex-tmp/base-repair-20260915/missing-openblas.log`。没有修改原 SIF。

B187-BASE 的独立出口完成。T001 的完整 pair 和 T003 的 YOLO/MiniNDN 仍未完成；后续 APP recipe 需要明确使用隔离 SDK，并重新绑定新 base 的摘要。未上传或提交 Tiger 作业。

额外执行旧入口检查 `tests/python/test_build_local_sif_record.py` 和 `tests/container/unit/test_spec170_exact_sif_gate.py`，结果 **6 failed, 11 passed**（原始 `existing-builder-tests.log`）。六个失败的首边界均为旧 fixture 缺少 `export NDNSF_NAC_ABE_PREFIX=/opt/ndnsf-stage`，被既有 validator 提前以 `WRONG_BUILD_BOUNDARY_PYTHON_NAC_STAGE_PREFIX_MISSING` 拒绝；其中本应检查后续 label/functorch/transfer 的断言也因此未到达。两个测试、旧入口和 validator 相对 HEAD 均无差异，且不调用新增 base 入口。这是已定位的旧 fixture/validator 不一致，不计为新入口 PASS，也不在本批顺带修改旧完整 APP 流程。

## Miss retrospective

static：历史脚本的 Waf 选项与当前源码不兼容、绑定依赖 DI、继承应用清理不足；按当前层次移除这些 base 中的应用构建职责。
compile-link：r1 编译前暴露父镜像缺少 g++，静态审查未发现；已补容器内工具链安装。
runtime-test：r2 C++ SDK/NFD/NumPy/ELF 检查及最终镜像复验通过；缺失 OpenBLAS 反例被拒绝。
unobserved：完整 APP、YOLO/MiniNDN 和 Tiger 验收均不在本次简单 base 测试范围。
