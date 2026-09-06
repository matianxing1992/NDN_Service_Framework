# Exact Data Wire Repair

**Evidence layer**: implemented / wired / executed (focused Core and DI repair)
**Status**: PASS (affected convergence and native identity; matrix pending)

## Baseline And Controlling Failure

基线 `b95b7e84`；R18 实际源码 `c0d585f1`。MANIFEST 与 SEG 的完整
签名 Data 超过 8,800 bytes，首次发送失败，后续依赖超时。详见
[R18](t005-formal-matrix-20260906.md#exact-dependency-boundary-r18)。
本记录属于 T005/A05 定向修复，不证明正式矩阵或 T008 资格。

## Repair Boundaries

Core 仅负责所有精确 Data 的完整签名 wire 大小检查：
`ServiceProvider::publishCollaborationSignedExactData` 先构造并签名整个
批次，任一对象超限就返回 false，全部大小检查通过后才插入 IMS。
不得以 content 长度代替 wire 长度；不改变包大小上限。该行为可以
独立验证和提交，但单独完成它不能使 R18 的 tensor 传输成功。

DI 负责紧凑表示及恢复。主工作区的候选改动仍为待审查状态：
`TensorBundleCodec`、`ProviderGroupCoordinator` 和
`NdnsfCollaborationDependencyIo` 必须保留签名原文、精确名称、
有序 ciphertext 承诺、AEAD/HMAC 和完整内容摘要的校验。
旧格式保持原始字段检查；只有紧凑格式才从已认证 Selection/能力恢复
省略字段。不能覆盖旧字段后再把自相等判断称为身份验证。

当前源码审查发现：候选消费代码无条件覆盖 inner manifest/descriptor，
随后无条件比较仅紧凑格式才有的 transportManifestDigest，破坏旧格式
兼容；候选外层获取尚需在分配/获取前检查 operation/edge/capability
的字节、分段数与算术边界。修复这些问题后才可采用候选代码。
通用层不纳入候选文件中的 YOLO Merge 名称特判。

## Focused Verification Plan

Core 使用真实 ServiceProvider、签名密钥、IMS 和 DummyClientFace
测试 8,799 / 8,800 / 8,801 字节的完整 Data：前两者可发布且可拉取，
后一者拒绝；批次末项超限时前项也不能出现在缓存。先在提交前实现
运行同一断言，再修复并重跑，保留独立 raw 目录。

DI 使用生产发布/消费路径验证大 tensor 的完整签名包大小、内容往返、
紧凑与旧格式兼容，以及签名/承诺/身份/边界变异拒绝。不能只检查
content 字节数或用手工恢复的测试副本代替真实消费路径。
随后刷新完整 native identity，复审 A05，再开始新的同源正式矩阵。

## Status

`PASS`（受影响收敛边界）：Core R2 21/21 断言 PASS；DI R6 9/9
用例、270/270 断言 PASS，完整 native 刷新和独立验证通过，同一
实际 Core 上重跑两组检查均通过。允许 R19；T005/T008 未完成。

## Core Regression R1

原始目录为 ignored workspace temporary directory 下
`spec181-exact-wire-core-20260906-r1/`；源码基线 `b95b7e84`，仅新增
测试及构建注册。系统 Python/Waf `--targets=spec181-exact-data-wire -j2`
构建 exit 0（19.284 s），链接已验证的生产 Core shared library。
以私有 HOME、memory PIB/TPM、未使用的 transport 运行
`--run_test=Spec181ExactDataWire --report_level=detailed --log_level=test_suite`。

测试 exit 201：18/21 断言通过，3 项语义断言失败（约 0.506 s）：
8,801-byte 对象错误返回成功，末项超限的批次错误返回成功，批次
前项仍能由消费端经真实 IMS 拉取。8,799/8,800-byte 的发布、完整
签名包大小、内容拉取及后续正常发布均通过。这不是构建或启动失败。
采用候选 Core 大小预校验后，以原样测试在新的 R2 目录验证。

## Core Repair R2

原始目录 `spec181-exact-wire-core-20260906-r2/`。仅采用 Core 的
PreparedData 批量预校验 hunk；所有对象先签名并测量完整 wire，任一
超限就返回 false，全部通过才插入 IMS。主工作区与隔离检出的该
方法字节一致。测试和构建注册与 R1 相同，测试未放宽。

相同 Waf target 重建生产 Core 与测试 executable，exit 0
（1m34.961s）。相同测试命令 exit 0：1 个生产路径用例、21/21
断言 PASS（约 0.750 s）。确认实际发送的 8,799/8,800-byte 包
可拉取；8,801-byte 包被拒绝；末项超限批次的前项不能拉取；
后续正常发布恢复成功。未运行完整本地清单或 MiniNDN。

Core 定向边界 PASS。新增测试同时注册到维护 integration-tests
以保留 T008 覆盖。DI 格式/消费缺口仍 BLOCK，完整 native identity
需在该共享传输修复后刷新；不能使用 R2 作为正式资格。

## Documentary Check

首次 inventory 渲染 exit 1：本新增记录缺少头部 Evidence layer，
触发 `ACTIVE_EVIDENCE_LAYER_MISSING`。这是文档元数据缺项，不影响
R2 运行结果；原始错误保留于 R2 的 `inventory-initial.log`。补充
显式层级和状态后重新生成、检查 inventory，不修改门禁脚本。

## Tensor Regression R1

隔离基线 `0c383f9d` 加新增生产 DI 大 tensor 回归，原始目录
`spec181-exact-tensor-20260906-r1/`。Waf target
`spec181-exact-tensor-transport -j2` 构建 exit 0（2m14.322s）。
私有 HOME、memory PIB/TPM 运行 `--run_test=Spec181ExactTensorTransport`
并保留 detailed report/test-suite log。RSA epoch-key wrapping、默认
HMAC 与生产 DI publishOutput 均真实执行；1,400,017-byte 内容按
7,000-byte 分段，预期 201 个 segment。

测试 exit 201，约 0.544 s：旧 manifest content 16,251 bytes，
完整 signed Data **17,546 bytes**，被已修复 Core 的 8,800-byte
边界拒绝，publishOutput 抛错。该语义 RED 证明只增加 Core 检查
不足以完成传输。下一步采用紧凑 codec 与经过修正的生产恢复路径；
旧默认 encoder 保持旧格式，紧凑格式仅由精确路径显式选择。

## Tensor Probe R2

原始目录 `spec181-exact-tensor-20260906-r2/`。紧凑实现重建 exit 0
（1m16.989s）；测试 exit 201，412/413 断言通过，约 0.861 s。
1,400,017-byte payload 与 201 个分段完整重建，所有实际签名包
大小通过；唯一失败为包数 404 而非 202。源码检查确认 fixture
已建立 provider peer 转发，测试又添加了手工转发，每包被重复
响应。R3 在自定义转发前断开 fixture peer bridge，保持 202 包
断言和全部内容/大小判据。此次失败是测试链路重复，不能记作
完整测试 PASS；保留运行和原判断。

## Tensor Repair R3 And Negative Probe R4

R3 原始目录 `spec181-exact-tensor-20260906-r3/`：只断开重复的
fixture peer bridge，构建 exit 0（17.233s），原大 tensor 判据
211/211 断言 PASS、exit 0，约 0.770 s。

R4 `spec181-exact-tensor-20260906-r4/` 增加旧格式和四个拒绝用例。
5/6 用例通过，239/240 断言通过，exit 201。大 tensor 实际共
202 个 signed Data，最大 **8,477 bytes**。篡改外层 Data 后用真实
Provider 重新签名，四个拒绝分别到达 producer signature、ciphertext
commitment、上下文 signature 和 mustFetch bounds；超限声明未发出
任何 SEG Interest。这些不是 NDN signer-name 提前拒绝。

唯一失败为旧格式 fixture 仍选用 7,000-byte 明文分段，旧元数据
令首段 Data 达到 10,555 bytes，发布被 Core 正确拒绝，尚未测试
旧 decoder。R5 仅将 legacy fixture 改为 19-byte 内容、7-byte
分段以进入旧格式消费；大 tensor 用例继续保持 1,400,017 bytes
和 7,000-byte 分段，大小门槛不变。

## Tensor Repair R5 And Authenticated Inner Rejections R6

R5 `spec181-exact-tensor-20260906-r5/` 构建 exit 0（18.956s），
6/6 用例、251/251 断言通过、exit 0；旧格式 19-byte 内容成功
重建，实际 4 包，最大 3,634 bytes。大 tensor 仍为 202 包、最大
8,477 bytes，没有缩小该用例。

R6 `spec181-exact-tensor-20260906-r6/` 在同一生产代码上补充内层
拒绝检查，构建 exit 0（20.112s），**9/9 用例、270/270 断言**
PASS、exit 0（约 4.424 s）。新增用例先以生产 encoder/sealer
生成包，再用真实外层签名与有效 ciphertext 摘要发布：损坏内层
HMAC 在 `NDNSF_DATA_V1 HMAC verification failed` 拒绝；错误索引在
`index/rank/size mismatch` 拒绝；旧格式错误 requestId 在
`segment inner manifest mismatch` 拒绝。错误实现不能仅靠外层
签名通过就得到依赖结果；旧字段也不能被恢复值掩盖。

受审投影基线为 `0c383f9d`：仅修改 TensorBundleCodec、
ProviderGroupCoordinator、NdnsfCollaborationDependencyIo 及头文件，
新增真实传输回归与 focused/full integration 构建注册。Core 保持
上一检查点。旧默认 manifest encoder/signingBytes 原文保留；新
精确路径显式选 context-compact。恢复时限采用签名 createdAtMs，
签名/有序 ciphertext 承诺通过后才解密；字节/分段边界在获取前
验证。通用层没有纳入主工作区的 YOLO Merge 名称特判。

隔离交付投影 NativeProviderHandler 的现有 maxSegmentSize 为
7,000 bytes，测试与之相同。主工作区未提交的 7,600-byte 默认值
不在本次投影中，不能以本记录为该配置背书。完整 native Provider
尚未按本修复重建；A05 在完整 source/build identity 更新前保持
BLOCK，R6 不能替代 T005/T008 正式矩阵。

## Native Identity R1 And Convergence Review

源码 `ce6a4ba0f07bbdbc954a667f7846e5348c8861da`，隔离检出无
tracked/index 改动。原始目录为 ignored workspace temporary directory
下 `spec181-exact-wire-native-20260906-r1/`。维护
`scripts/spec180_native_build.py build --jobs 2` 与独立 `verify` 均
exit 0 / `SPEC180_NATIVE_IDENTITY_OK`。Core/Provider Waf 阶段
2m19.324s；Python 扩展重新构建（binding_reused=false），扩展最终
字节与先前相同，不据此跳过本轮源码/依赖核验。

| Artifact | SHA-256 |
|---|---|
| native build receipt | `2f4d7e61c6b0406b667af0048d582b5e91165f1823276b691191d1961ee48bd2` |
| Core shared library | `0e748103217e3f5af7038cc15d8aee0863e3c832a954ba9b7338df2d1d7d5370` |
| native Provider | `d279d40a699d1386d8cd589a613253265f5adcfebf87bd402f0eae24920eabb9` |
| Python extension | `e9932073205b66b79e12c6f3944342d65d91fdb340b3225eb4b3326f480e1339` |

系统 Python 3.8、系统编译/链接器、选定 ndn-svs build tree、实际
loaded mappings 由 receipt/verify 核对。新构建后再次运行
`spec181-exact-data-wire --run_test=Spec181ExactDataWire` 和
`spec181-exact-tensor-transport --run_test=Spec181ExactTensorTransport`，
均 exit 0：21/21 与 270/270 断言通过。日志为本目录
`focused-core.log`、`focused-tensor.log`，同样保留 detailed report。

按 post-implementation 的 12 维度复审受影响边界：

| Dimension | Verdict | Evidence / boundary |
|---|---|---|
| Intent fidelity | PASS | 保留 T005 七子用例和本地责任边界 |
| Necessity and scope | PASS | R18/R1 真实超包失败；紧凑表示有生产消费者 |
| Architecture and ownership | PASS | Core 仅负责 signed Data 大小；DI 负责 tensor 编解码 |
| Cross-artifact consistency | PASS | spec 纳入 exact-tensor-wire 契约，tasks/evidence 同步 |
| Code reality | PASS | 已提交 publish/prefetch、默认 7,000-byte 分段、真实库与入口 |
| Security and correctness | PASS | 旧签名原文、外层承诺、内层 HMAC、上下文与索引拒绝 |
| Task executability | PASS | Core 大小和 DI 传输分别可审阅；仍归同一 T005 修复 |
| Validation design | PASS | 语义 RED/GREEN、九项真实用例；矩阵验收仍待执行 |
| Evidence integrity | PASS | R1–R6 分开保留，环境失败与业务拒绝分开，最终构建身份明确 |
| Migration and rollback | PASS | 旧 decoder 兼容已执行，新 marker 要求同版本接收端；未混入预存改动 |
| Performance and operations | PASS | 只作有限功能验证；不提升超时或 packet limit、不声称性能收益 |
| Documentation quality | PASS | Spec181 显式路径结构检查、prerequisites、证据清单可核对 |

当前控制性代码/构建缺口关闭，A05 恢复 PASS，允许新 R19 正式
矩阵。这里的 PASS 是执行前收敛许可，不是 T005/T008 资格结果。
主工作区另一个任务已把活动指针设为 Spec182；本轮用显式 Spec181
文档和 Spec181 隔离源码复审，未改写该指针。T008 的 19 个额外
Python 测试来源差异仍须按现有任务核对，不能遗漏后声称完整清单。
