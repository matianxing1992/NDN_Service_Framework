# Exact Tensor Wire Representation

## Scope

本契约补充继承的 Spec170 `hybrid-execution-v1.md` 精确 tensor 传输，
修复 R18 包大小问题。它规定相同逻辑对象的新紧凑 wire 表示，保留
原精确名称、逻辑 manifest、签名原文及内容校验。仅适用于共同更新
到本 Spec181 版本的本地生产者/消费者；旧二进制不保证读取新 marker。
当前 decoder 继续接受旧格式并验证旧字段；不静默降级为不受保护路径。

## Representation And Authentication

`TensorObjectManifestV1ContextCompact` 保存 contentDigest、totalBytes、
segmentSize、segmentCount、ordered ciphertext digests 的 SHA-256 承诺、
createdAtMs 和 producerSignature。承诺输入是按序拼接的规范
`sha256:<64 lowercase hex>` 字符串。其余字段只可从已认证 Selection
edge 和 group capability 恢复；获取所有分段并计算其 wire 摘要后，
恢复原 orderedSegmentDigests，再以旧版 signingBytes 原文验签。
不得在恢复和验签之前暴露明文、接受分段或更新依赖就绪状态。

`NDNSF_DATA_V1_SEGMENT_BUNDLE_COMPACT` 保存原 operation manifest 的
32-byte digest、numeric producerRank、segmentNo、nonce、ciphertext、
authTag 和 HMAC。完整精确 Data 名由已认证边及请求的分段索引确定。
其他 descriptor 字段从验证后的外层 manifest/edge/capability 恢复。
仅该格式省略重复的内层明文摘要列表与其 signature；已认证外层的
有序 ciphertext 承诺、原 operation digest 绑定的 AEAD/HMAC 和最终
contentDigest 共同验证完整对象。Coordinator 的该调用约定必须显式，
不得让未认证分段进入省略内层签名的处理路径。

旧格式保留原 manifest 签名和每段摘要验证，禁止先覆盖其已传输字段。
紧凑格式的 rank/index/digest 等已传输字段同样先核对，不能被恢复值
掩盖。恢复时间使用生产者已认证的 createdAtMs，不以接收时间重置
原来的时限。任何格式的失败均不能产生完整依赖结果。

## Bounds And Publication

Core 批次所有对象签名后，以完整 Data wire 判断是否超过
`ndn::MAX_NDN_PACKET_SIZE`；一项超限就拒绝，大小预检完成前不插入 IMS。
DI 在获取分段或按声明分配前检查 segmentCount、totalBytes、segmentSize
及精确分段算术关系，受 edge、operation、capability 的现有上限约束。
分段总数、索引、累计 ciphertext/plaintext 字节和最终内容长度均有界；
不得以无限缓冲或提高 8,800-byte 上限补救。重复分段遵循既有 replay
window；整个 tensor 的 digest 与完整 bitmap 通过后才返回。

## Implementation And Proof

修改 owner：Core `ServiceProvider::publishCollaborationSignedExactData`；
DI `TensorBundleCodec`、`ProviderGroupCoordinator`、
`NdnsfCollaborationDependencyIo::publishOutput/prefetchInput`。
encoder/decoder 的新 marker、瞬态恢复字段和逻辑签名字段严格区分。
同源生产集成验证涵盖长名称/大量分段、实际 signed Data 大小、
旧格式兼容、错误签名/承诺/上下文/长度与乱序索引拒绝。
Core 定向检查已归独立检查点；DI 修复与验证仍在进行，见
[wire repair](../evidence/t005-exact-data-wire-repair-20260906.md)。
