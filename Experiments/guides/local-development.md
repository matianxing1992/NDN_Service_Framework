# 本地构建、安装与依赖边界

## 本地构建树与并发

NDNSF用根Waf。宿主复用 checkout 内两棵维护树：`build/`（普通构建、发布候选、全局安装）与
`build-ASAN-UBSAN/`（ASAN/UBSAN 诊断与测试），各自持有独立的 Waf 配置/签名状态、对象文件与生成头文件，
通过各自保留的 `WAFLOCK` 显式选择；只有普通树可以全局安装。外部依赖各用其维护系统。
先检查已有Waf配置/lock和实际out_dir，保留WAFLOCK；不因一次小改重配全仓库，
不另建按Spec命名的build/install树。

~~~bash
scripts/build-safe.sh build
scripts/install-global-target.sh --target ndnsf-distributed-inference
~~~

物理内存不超过 12 GB 时默认并封顶四路，12 GB 以上且低于 100 GB 时默认并封顶六路，
内存未知时沿用六路。至少 100 GB 时默认使用 `nproc`，允许显式指定正整数。
构建沿用宿主内存限制和 Waf 并发调度；
发生实际OOM或用户要求时才降低并发。保留进程组所有权、build锁和失败回收，
不修改 MemorySwap 或系统 swap。`scripts/build-safe.sh` 负责构建锁和磁盘准入。

按改动构建/验证必需目标；库、应用和实际交付的bindings需要同源。
公共头文件/ABI改变可能扩大重建范围；读源码、改文档不需要启动产品构建。
全量安装不等于所有测试、推理或发布资格通过。

## 依赖和实际装载

安装/配置说明见[installation](installation.md)；先查看只读计划，确认系统、SDK来源、
完整commit/摘要和ABI，再执行授权的安装。使用已声明的全局prefix，不从临时Spec树、
旧stage目录或宿主venv夹带库。编译时include/pkg-config、链接时库和运行时loaded库都要核对。
相同SONAME、mtime或strings中出现一句话不是二进制身份/ABI证明。

配置匹配时，整栈和单目标安装都复用 `build/`。安装入口验证最终文件和可用入口后，
回收 `build/install-stage`。用维护的打包和清单工具生成 release，
不手工改manifest里的全部hash来让错候选通过。

## 与容器的分界

宿主二进制不能复制成容器运行闭包。SIF中的C++入口在候选或ABI一致builder内构建，
核对SDK、ELF、RPATH和真实运行库；见[Tiger SIF](../TigerCluster/docs/sif-build.md)。
仅模型/节点/输入变化不重建SIF；容器源码或ABI变化才回到受影响构建。
容器使用Slurm实际分配的CPU，先确认nproc/affinity，再决定jobs，不沿用宿主六核或写死两路。

## 验证与临时目录

C++行为用生产target的原生fixtures；host Python可编排，不重写协议或oracle。
共享CPU/CUDA机制先focused native及installed MiniNDN CPU冷暖，再做Tiger。
使用带owner/锁的磁盘/tmp目的目录，容量检查后创建；禁止把模型大小的产物放/dev/shm。
模型、Repo、凭据与证据在scratch外。归档后回收本任务临时对象；不 blanket clean其他工作区。

本页收敛旧 build-and-test、native-build-parallelism 和 native-dependency-closure
的现行规则；历史主机库SHA、临时构建路径和旧测试计数不作为新环境默认值。
