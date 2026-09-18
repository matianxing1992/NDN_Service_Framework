# Native Build Parallelism

## Current Development Host

2026-09-07 用户将开发机调整为 6 核、12 GB RAM，并授权后续原生构建默认 `-j4`；
2026-09-08 再次确认该默认值。执行者不得仅因旧 evidence 使用过 `-j2` 就沿用降档。
实查 `nproc` 为 6，`free -h` 显示约 11 GiB 可用物理总量（界面单位及取整），swap 总量 8 GiB。
这是当前开发机资源策略，不是已经测得的最佳并行度；不自动外推其他机器或 SIF builder。

## Execution Rule

- Waf 使用 `./waf build -j4 --out="$BUILD_DIR"`，target 和 build 目录仍按当前任务冻结入口。
- CMake 使用 `cmake --build "$BUILD_DIR" --parallel 4`；具体工具链/prefix 沿原生 preflight。
- 同一 Waf 树只允许一个构建进程；也不同时启动多个各自 -j4 的原生依赖构建。
- 观察 `MemAvailable` 和 `vmstat 1` 后续行的 `si/so`。持续换页或桌面明显卡顿时，
  当前进程由其执行者安全收尾/停止；只有确认持续换页或桌面卡顿时，下次调用才降为
  `-j2`，否则恢复默认 `-j4`；不为调并行度清空构建目录。
- 沿用 system compiler/binutils、匹配 Boost/NAC-ABE/SVS 与 ABI 闭包要求。
  修改并行度不能修复缺头文件、缺符号或 ABI 错配；ABI 变化仍需按原规则重建依赖消费者。

## Target Scope

`integration-tests` 与可安装 DI library 共享递归的 DI core/adapters 源闭包。
因此，新增 native translation unit 时不再维护第二份手工测试清单；在已验证树中按需选择
`integration-tests` 或受影响的 DI target。只有共享头、生成输入、构建配置、ABI 或依赖变化
才扩大到 Core、Repo、UAV 等传递消费者，并在 Spec 证据中记录边界。

安装也沿用同一个 target 边界。不要为了更新一个库从仓库根重新选择 Waf 输出树，也不要
用无目标的 `install` 把所有已配置目标重新编译一遍；应从产生该库的已验证 build tree
执行目标化构建和安装：

```bash
cd /absolute/path/to/build-spec189-b189-3-global-r3
../waf build --targets=ndnsf-distributed-inference -j4
sudo -n ../waf install --targets=ndnsf-distributed-inference -j4
```

Core、Repo 或绑定只有在其源码/ABI 确实受影响时才分别选择对应 target。ONNX Runtime
是已安装的全局版本化 SDK（当前 `/opt/onnxruntime`），不是每次 DI 增量构建都要重新
编译的仓库目标；DI 只重新链接受影响的 ONNX 使用者，并在安装后用 `sha256sum`、
`readelf -d` 和 `ldd` 验证它仍解析到声明的全局 SDK。目标化安装不改变 ABI 一致性要求：
共享头、依赖版本或 SONAME 变化时，必须扩大重建范围并重新安装所有传递消费者。

## Why Time and Memory Grow

2026-09-08 用户强调只重建受影响模块。日常批次复用同一已验证配置的 build tree，
显式选择目标；DI 改动不清空 Core/UAV 对象、不因新 batch ID 创建 fresh build。
只有实际依赖变化（共享头文件、编译配置、ABI）才重建相应消费者。需要产出 DI 库时
选择 `--targets=ndnsf-distributed-inference`；需要验证单测时选择 `--targets=unit-tests`，
由 Waf 依赖图重编失效对象并链接，不能用“只编库成功”代替单测。
Waf 的 `[n/total]` 是任务图编号，不是实际编译次数；报告 Compiling/Linking 实际行。
R1-B2 首轮只编 DI preparation 与其测试，次轮只编测试；Core/UAV 没有重编，
分别 16.928s/16.033s。原始日志见当前 Spec 的 R1-B2 证据；不作为 fresh build 测量。

`-j2` 最多并行调度两个构建任务，不会把一个 C++ 源文件自动拆给多个核。
大型 C++ 单元的头文件解析、模板实例化和优化需要时间及内存；多个编译进程叠加后峰值会增长。
链接和依赖顺序限制可并行部分。编辑器、索引器及其他应用也共享物理内存。
因此 -j4 有望缩短可并行阶段，但不能保证相对 -j2 加速两倍，也不能保证内存峰值不超预算。

## Native dependency closure

The current host uses Boost 1.71 from `/usr/include` and
`/usr/lib/x86_64-linux-gnu`, and the canonical installed `libndn-cxx`/NFD pair
from `/usr/local`. The repository `.local-boost171` tree contains a different
same-SONAME build and is historical staging; Waf ignores it by default and the
host MiniNDN runner rejects it before startup. An explicit NDN-SVS or NAC-ABE
development prefix may still be selected, but its transitive `libndn-cxx` must
resolve to the same real file and digest as NFD. See
[Native Dependency Closure](native-dependency-closure.md) for the identity table
and `ldd`/SHA-256 checks.

## Observation and Limits

本次只做只读快照，没有启动、停止或重启任何构建。
现场已存在 `python3 ./waf -o build-nac182 build -j4`，并观察到 4 个 cc1plus。
一次进程快照中两个较大 cc1plus 的 RSS 分别为 1,778,736 KiB、1,209,756 KiB，
CPU 使用率约 94%、91%；另有编辑器和索引器等进程。不能由快照断言整次耗时都花在这些进程。
`free -h` 快照约 6.5 GiB used、4.6 GiB available、868 MiB swap used。
`vmstat 1 4` 丢弃首行后，三行 si/so 为 4/0、0/0、0/0，未见这三秒内持续换页。
已有 swap 占用不等于当前发生 swap thrashing；这些短样本不证明全程峰值或稳定性。
未做同源码/同缓存条件的 -j2/-j4 对照，不能报告具体加速比。

## Evidence Policy

未来正常构建记录真实命令、构建目录、源码身份、耗时及资源观测；只有同等重建范围才比较速度。
旧证据中的 -j2 命令保持历史事实，不批量替换。仅当前执行指引和后续命令采用 -j4。

本次文档检查：Spec182 design validator PASS（36 进度行、父任务 5/17，状态由并行实施维护），
`git diff --check` PASS。初次校验发现既有 T002-A failure-log 链接多一级 `../`，
修正为 `../../docs/failure-log.md` 后通过；只修路径，不改验收状态。
AGENTS.md/CLAUDE.md 为本机指令文件，不纳入 Git；本规则文档、plan 和执行契约随仓库保存。
