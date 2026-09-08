# T003 Ordinary Candidate Publication

## Changes and Review

基线 5adc6688。沿实际 NativeRequestPreparation 发布链审查发现，validateRoles
接受普通候选省略两个 rank map，但 ensureArtifacts 用 map.at 强制读取 degree，
会在 ArtifactPort 调用前抛 out_of_range。统一为已验证候选的 implicit degree=1，
保留 selectedRole/provider/offer 绑定检查。既有准备流程测试增加省略 rank map 的
候选，直接调用生产 ensureArtifacts，要求实际到达发布 port 并返回同一制品身份。
同类审查还发现 NativeCanonicalArtifactPublisher 的角色校验和制品名称生成也强制
读取该映射，一并修复。实际发布器回归覆盖 inline/external source × implicit/explicit
rank-one 四种组合，检查发布内容、根摘要、制品名称和发布后的角色认证。
NativePlanning 中剩余 map.at 均受完整 map cover 或显式 contains 检查保护。
本变更不改变结构布局；源码和测试静态审查完成。

## Validation

PARTIAL。复用已核对 ABI tree、system g++/ld 与 Boost 路径，串行执行两次增量
-j4 build PASS，最终发布器 build 26.196s。十二组 focused suite：114/114 cases、
2248/2248 assertions PASS。vmstat 第二次采样 si/so=0。没有完整 requester、网络或资格验收结论。
Context Mode active health 因 tasks 索引过期 exit=4，采用仓库文档和实时源码；
CodeGraph 返回的临时 staging 副本被排除，定位后只核对 canonical 源文件。

Evidence: [build](../../../.codex-tmp/spec182-preparation-rank-r1/build.log)、
[publisher build](../../../.codex-tmp/spec182-preparation-rank-r1/build-publisher.log)、
[focused](../../../.codex-tmp/spec182-preparation-rank-r1/focused.log)。
