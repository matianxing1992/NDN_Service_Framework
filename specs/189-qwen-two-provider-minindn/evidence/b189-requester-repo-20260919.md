# B189 Requester Repo Integration

## Scope and design binding

T003 / B189-1b；遵循 `contracts/model-preparation.md#protected-repo-integration-binding`。
实际 requester 的可选 `repository.path/max_bytes` 配置接入 RepoSourceProvider，
只承担 canonical source lookup/ingest。`encrypted_repository` 继续承担 ciphertext
存储；Runtime 的 Core ServiceUser 仍负责 protected publication 和 NDN serving。
本单元不引入另一个网络 publisher；不将本地文件 receipt 当作远端可读证明。

Batch growth decision：修复首次 source miss 下 external initializer 的重复 fallback，
因为该错误由实际 Qwen requester 接入同一 source owner 直接触发；范围止于 source
ingest、requester 配置及 C++ 回归，不吸收 ACK/handoff/oracle 工作。

## Review

Base `61d69ae0`。只读审查 agent `/root/spec189_review2` 检查冻结 r1–r4。
r1 发现本地 publisher 错误替代 NDN publication，已删除该赋值；r2 发现首次
source miss 重复读取 graph/initializer，已复用首次 fallback 的 initializer。
r4 对组合生产/调用方/回归测试返回 STATIC_PASS；原快照保留于
`.codex-tmp/spec189-qwen-repo-requester-review-r4/`。

| Lane | Actual scope |
| --- | --- |
| Production/callers | DI_NativeRequester.cpp RuntimeConfig；Qwen MiniNDN requester 配置 |
| Implementation/wire | RepoSourceProvider::load；Runtime::User::prepare publication 分支 |
| Test/oracle | Spec189RepoPublication/RepoSourceMissReusesValidatedInitializer |
| Build/source | examples/wscript DI_NativeRequester 的 Repo/DI/Core link closure；原 unit-tests |
| Migration/evidence | 配置可选；protected publication 不变；本记录和 tasks.md |

r4 diff SHA256 `dc14ffc20b16f191b369eed1d92276140132549d0215feca7a06204dedf38b9e`。
快照含这两个此前已有修改的调用方完整差异，checkpoint 必须单独核对既存改动归属。

## Validation

2026-09-19 01:11 -05:00：原构建会话 86561 经中断后重新轮询确认仍在运行。
命令 `../waf build --targets=unit-tests,DI_NativeRequester -j1`；日志
`.codex-tmp/spec189-qwen-repo-requester-build-r1/build.log`。
沿用中断前已启动的单进程构建，没有启动竞争构建或重编 ORT。
该构建随后受控中断（rc=68），未运行新增 C++ 断言，状态 PARTIAL。
补查本机 Boost 1.71 的 `print_helper.hpp` 发现 r4 两处 vector
`BOOST_CHECK_EQUAL` 要求不存在的 ostream operator；另发现恢复 initializer 分支
的一次多余复制。因此 r4 STATIC_PASS 已撤回，等待 r5 组合复审；这不是已观察到
的 compiler failure，而是编译中补查发现的静态漏检。Changed gate：检查实际
Boost 宏实例化/打印要求，并逐分支核对 optional<vector> 的 copy/move。
r5 已改 collection assertions 和 move，diff SHA256
`e3e9a9daa925edb5d58fbe6ea7025754e9c6929f4f724140cb7337fad80c0a41`。

Context Mode active health：缺失项目 ContentDB（exit 5），使用仓库文件与原始日志回退。

## Closure and retrospective

r5 组合门 STATIC_PASS 后续编 `unit-tests,DI_NativeRequester -j4`，r2 构建
rc=1：requester 的 `makeFilesystemRepoStore` 缺少声明头。首次边界是编译，
不是 Repo/NDN 运行失败；原始日志 `.codex-tmp/spec189-qwen-repo-requester-build-r2/build.log`。
Changed gate：factory 声明 `FilesystemRepoStoreBackend.hpp` → 定义
`NDNSF-DistributedRepo/src/backends/FilesystemRepoStoreBackend.cpp` →
`ndnsf-distributed-repo` → `examples/wscript` 的 DI_NativeRequester `use`。
加入声明头后需重新审查并增量续编，不能沿用 r5 的 build lane 结论。

## Verified result

2026-09-19 01:17 -05:00：r6 声明/定义/target 组合复审 STATIC_PASS；
`nm -C` 确认 build tree 的 Repo 静态库定义了 makeFilesystemRepoStore。
同树 r3 `unit-tests,DI_NativeRequester -j4` 构建 PASS（44.725s，前两次的编译耗时
不包含在此数字中）。r5 Repo/test 和 r6 requester 文件摘要与实际工作区一致。

仓库根执行：

```text
unit-tests --run_test=Spec189RepoPublication --log_level=test_suite: 5/5 PASS
unit-tests --run_test=Spec185Runtime/PrepareSuccessUsesTheProductionRuntimeEntry --log_level=test_suite: 1/1 PASS
```

第二条首次因 root:root 0755 的默认 `/tmp/ndnsf-large-data` 不可写而 rc201。
环境修复经只读复审：命令作用域设置 NDNSF_REQUEST_LARGE_DATA_DIR 到本 run 下
tianxing:0700 的 `runtime-spool`，寿命覆盖整个测试进程。相同二进制重跑 PASS；
没有改默认目录、生产权限或测试断言。完整日志位于
`.codex-tmp/spec189-qwen-repo-requester-build-r3/`，保留两次 Runtime 输出。

| Artifact | SHA256 |
| --- | --- |
| unit-tests | 1725a147cc12a3a12d60113ebe88a9a1cdc73a3dd5c0e799ebe7678afd29e531 |
| examples/DI_NativeRequester | 126d2ac85e760244b7a3757ed8793393bf6aae28746777c7c709faee09cf592c |
| RepoSourceProvider.hpp | 0a437f624e684b9474ad5c4c41a2d01efec65d750ae8743d67d67f5bba47df7f |
| spec189-repo-publication.t.cpp | 7622506f366a3f1bb6079fcdfac48aa7edae933515b1322aef1dc50e58179739 |
| DI_NativeRequester.cpp | d5a8a4d9d8d12e9985bae061af2045bb2b35ad8a8313fad3624334f62c0277dc |

Dynamic profile：none（本单元是同步 source ownership 和调用次数反例，没有新增并发
机制）；真实大模型峰值和资源 drain 仍交 T009，不以小 fixture 结果作资源资格。
Python runner 仅完成 AST 语法检查；未执行模型或 MiniNDN。

Retrospective：static 拦截错误 publisher 和重复 fallback；补查修复 Boost 宏/复制；
compile-link 漏检 requester 声明头；runtime-test 首次失败是 fixture spool 权限；
unobserved 为真实跨节点读取、Qwen 两 Provider、同 handle 两请求、独立输出和 drain。

Closure：本地 Repo 修复与 Runtime source-owner 回归通过；T003 仍 PARTIAL。
requester `ldd` 解析 DI 为 `/usr/local/lib/libndnsf-distributed-inference.so`，
ORT 为 `/opt/onnxruntime/lib/libonnxruntime.so.1`。尚未安装新 DI 全局库，不能用本次
requester 链接成功宣称完整候选可运行；MiniNDN 前必须单目标安装并核对加载摘要。
requester/Python 调用方有既存并行修改，本 checkpoint 只收录独立 Repo/test 修复、
本记录和本轮进度段，调用方待关联构建/部署单元统一归档。

## Global installation follow-up

2026-09-19 01:23 -05:00：已完成单目标安装并重新核对加载路径，前述“尚未安装”是
该 checkpoint 时的历史状态，当前安装缺口已解除。

```bash
scripts/install-global-target.sh --build-dir build-spec189-b189-3-global-r3 \
  --target ndnsf-distributed-inference --jobs 4
```

原始记录 `.codex-tmp/spec189-global-di-refresh-20260919-r1/install.log` 与
`install.rc`（0）；Waf install 为 3.663s，不是完整构建耗时。依赖预检通过，Waf
同时处理 Core 传递依赖；本命令没有重编 ONNX Runtime 或 Python bindings。

| Library | Matching build/global SHA256 |
| --- | --- |
| DI | `18407813929c245e45bca93d352396fe181efc9327503c722e8edead14bee924` |
| Core | `529c798d651e86e24ee7eb28af6dbf6cb7478b0c6dedae3a50e6eebaedc534f8` |

requester 的 `ldd` 再次解析 DI/Core 到 `/usr/local/lib`、ORT 到
`/opt/onnxruntime/lib`，没有 missing library。此项证明安装身份和动态链接解析，
不证明真实跨节点读取、两 Provider 推理或 MiniNDN 资格。
