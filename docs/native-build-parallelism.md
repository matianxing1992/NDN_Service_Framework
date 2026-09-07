# Native Build Parallelism

## Current Development Host

2026-09-07 用户将开发机调整为 6 核、12 GB RAM，并授权后续原生构建默认 `-j4`。
实查 `nproc` 为 6，`free -h` 显示约 11 GiB 可用物理总量（界面单位及取整），swap 总量 8 GiB。
这是当前开发机资源策略，不是已经测得的最佳并行度；不自动外推其他机器或 SIF builder。

## Execution Rule

- Waf 使用 `./waf build -j4 --out="$BUILD_DIR"`，target 和 build 目录仍按当前任务冻结入口。
- CMake 使用 `cmake --build "$BUILD_DIR" --parallel 4`；具体工具链/prefix 沿原生 preflight。
- 同一 Waf 树只允许一个构建进程；也不同时启动多个各自 -j4 的原生依赖构建。
- 观察 `MemAvailable` 和 `vmstat 1` 后续行的 `si/so`。持续换页或桌面明显卡顿时，
  当前进程由其执行者安全收尾/停止，下次调用降为 `-j2`；不为调并行度清空构建目录。
- 沿用 system compiler/binutils、匹配 Boost/NAC-ABE/SVS 与 ABI 闭包要求。
  修改并行度不能修复缺头文件、缺符号或 ABI 错配；ABI 变化仍需按原规则重建依赖消费者。

## Why Time and Memory Grow

`-j2` 最多并行调度两个构建任务，不会把一个 C++ 源文件自动拆给多个核。
大型 C++ 单元的头文件解析、模板实例化和优化需要时间及内存；多个编译进程叠加后峰值会增长。
链接和依赖顺序限制可并行部分。编辑器、索引器及其他应用也共享物理内存。
因此 -j4 有望缩短可并行阶段，但不能保证相对 -j2 加速两倍，也不能保证内存峰值不超预算。

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
