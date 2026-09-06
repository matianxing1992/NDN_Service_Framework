# NAC-ABE 与官方分支比较

> 这是合入前的比较快照。随后已按用户授权合入官方更新，保留本地修复，
> 并通过分段和授权回归；最新结果见 [T022 验收](nac-abe-official-merge-20260905.md)。

核验时间：2026-09-05 CDT。用户要求不提交 PR；本次没有 push，没有修改
实际 NAC 分支，只进行了远端读取、源码比较和隔离合并预演。

## 结论

**建议同步官方新增修复，同时保留自己的改动。** 你的仓库与官方已经分叉，
不能直接重置为官方内容，否则会丢失当前 NDNSF 所需接口、修复和测试。

官方是 [UCLA-IRL/NAC-ABE](https://github.com/UCLA-IRL/NAC-ABE)，默认分支
`master`。GitHub repository API 的 `parent`/`source` 确认你的仓库和
`suraviregmi/NAC-ABE` 都是它的 fork。之前把 suraviregmi 称为原项目不准确。

## 提交关系

| 分支 | HEAD | 相对官方独有提交 | 官方独有提交 |
|---|---|---:|---:|
| 官方 `UCLA-IRL/master` | `58f394862cd2a2462fbcf763c000c745f9f7f0c8` | 0 | 0 |
| 你的远端 `master` | `1cc17d9d21f4dfc0921cc77315d0c57d46291880` | 2 | 3 |
| 你的本地 `Experimental` | `85547eb558c4a4f706b51cb354ab4db573609895` | 6 | 3 |
| `suraviregmi/master` | `5ac3eb991d6ed7eef36e6a265e97912961e9807f` | 0 | 13 |
| `suraviregmi/ck-duplicate-interest` | `b69dc2e4aa2ce146605728c3a19f7e32e65e4431` | 0 | 1 |

你的两个分支与官方共同祖先：`c56b70d819d4649d35028445675c91ac435a30fc`。
官方与 suraviregmi 的 ck-duplicate-interest 文件树相同，多出一个合并提交。
此前“官方缺少12个前置提交”实际是与过时 fork master 比较，不能用于官方判断。

## 官方有、你没有的修复

| 官方提交 | 变更与影响 |
|---|---|
| [8439586](https://github.com/UCLA-IRL/NAC-ABE/commit/84395860316b4db737d70886aaf8dab90637f3ef) | CacheProducer 的 CP/KP 两条路径向 ckDataGen 和 Producer::produce 传递 maxSegmentSize。当前本地接受该参数却不传下去，非默认值会被忽略。建议合入并验证两种模式的冷/热缓存。已有 CK 不会仅因后续调用改变分段大小而自动重新分段。 |
| [b69dc2e](https://github.com/UCLA-IRL/NAC-ABE/commit/b69dc2e4aa2ce146605728c3a19f7e32e65e4431) | 移除数据名称末尾 segment 裁剪与 normalizeCkKey，直接使用 CK 名称；部分 INFO 降为 DEBUG。名称语义与缓存去重键会变化，需验证精确段名、对象名、CK 共享/去重及大载荷，不能只当作日志清理。 |
| [58f3948](https://github.com/UCLA-IRL/NAC-ABE/commit/58f394862cd2a2462fbcf763c000c745f9f7f0c8) | 合入上述分支的 PR39，文件树与 b69dc2e 相同。 |

两个实质提交只修改 `src/cache-producer.cpp`、`src/consumer.cpp`，没有新增
测试。当前实际分支和已测试 prefix 尚未包含它们，之前的全部通过结果不能
解释为已覆盖官方最新修复。

## 你的独有改动

master 多出708a81e（OpenABE Python 退出处理）和1cc17d9（加密/集成测试）。
Experimental 另有 b1c9c4f、8b462d0、b3b43c8、85547eb，包含 DKEY freshness、
权限策略/代际 API、刷新/失效接口、迟到回调隔离、缓存权限绑定、重入修复、
精确参数校验以及 ABI 重建说明。官方没有这套完整契约，应保留。

官方最新版本本次未另做 NDNSF 编译。缺失接口来自源码比较，RV-U22 的既有
编译失败是在旧 fork 基线上测得，不能称为对官方最新 HEAD 的编译实测。

## 合并预演证据与边界

从交付 bundle 恢复独立临时仓库，抓取最新官方 refs；分别执行
`git merge --no-commit --no-ff compare-official/master`：

| 起点 | 结果 | 候选文件树 |
|---|---|---|
| master 1cc17d9 | 无文本冲突，仅变更上述2个源文件 | `eaa0a7820f37be02b4c59e21d032a67fb89dc3d0` |
| Experimental 85547eb | 无文本冲突，仅变更上述2个源文件 | `78107991cbc40466a9876cdb61e320370ad6d0f6` |

逐块 diff 保留了已有 generation 检查、回调重入和其他 NAC 修复。这是静态
合并证据，**没有编译或运行候选版本**。预演已分别 merge --abort，临时目录
恢复干净，实际 NAC HEAD 仍为85547eb；没有实际合并提交。

复现比较使用最新 refs、`git rev-list --left-right --count A...B`、
`git merge-base`、`git log A..B` 和具体文件 diff，未依赖旧 remote-tracking refs。

下一步建议：独立候选分支合入官方58f3948，补非默认分段大小/名称语义测试，
运行 NAC 全套与 NDNSF 授权、分段及大响应回归。通过后再更新实际依赖，使用
新 prefix 和干净匹配构建。保留之前冻结的验收证据，不提交 PR、不自动推送。

工作流：Context Mode/Spec Kit/GSD 检查沿用并复核；NAC 无 CodeGraph 索引，
直接检查 Git 历史/差异。工程分支比较无需 ARS。T014 的外部发布验收仍未关闭。
