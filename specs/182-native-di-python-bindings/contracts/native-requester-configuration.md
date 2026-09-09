# Native Requester Configuration

## Status and Entry

R5-B5 / LOCAL_NATIVE_COMPOSITION_VERIFIED。源码入口为 `examples/DI_NativeRequester.cpp`，Waf target 为
`DI_NativeRequester`，链接 `ndnsf-distributed-inference`；CLI 先由 native loader 完成 catalog、
grant 和 admission 组合，再把 request fields 交给 `nativeRequestRuntimeFromJson` 做统一 runtime
schema/identity/budget/state 校验；已构建并验证 help/usage/错误schema，
真实模型与网络请求尚未验收，详见 [本地结果](../evidence/r3-b1-request-lifecycle-20260908.md#final-local-result)。

```bash
DI_NativeRequester --help
DI_NativeRequester --config requester.json --input application-input.bin --output result.bin
```

成功返回0；参数错误返回2；请求/配置失败返回1；SIGINT/SIGTERM 经 handle.cancel 后返回130。
输出文件只在请求成功后写入。input/output 路径相对当前工作目录，其余配置中的文件路径
相对配置文件所在目录，也可使用绝对路径。没有 Python bootstrap 或第二份 DI 实现。

## Configuration Owners

顶层 schema=`ndnsf-di-native-requester-v1`。所有摘要与 schema 身份必须来自已固定的
模型目录、源码包或运维配置；不能临时 hash 一个名称来代替缺失契约。

| Object | Required fields / meaning |
| --- | --- |
| core | group、requester_identity、authority_identity、trust_schema_file；authority_identity 是 Core AA，不是下行 grant issuer 的身份 |
| limits | bootstrap_ms（1..3600000）、max_source_bytes、max_assembled_bytes |
| catalog | 以下 catalog schema；包含模型与已有 Qwen/YOLO splitter 配置 |
| grant | authority_identity、requester_private_key_file、authority_private_key_file、content_key_id、content_key_file、protection_epoch、recipient_public_key_files |
| offer_admission | policy（既有 offer policy JSON）、public_key_files（signer key ID→PEM 文件）、candidate_digest |
| request | service、task、adapter_composition_digest、task_descriptor_digest、input_layout_digest、security_policy_digest、max_candidates、max_policy_ms、timeout_ms、ack_timeout_ms；可选 generation_mode、max_reentries、no_progress_ms、max_segments、options_file。未提供后三个 runtime limit 时由 CLI 使用受限默认值，再由 native parser 校验 |
| conversation | 可选的 `ndnsf-di-native-conversation-v1` owner 配置；由 C++ 读取 operator-owned journal/key files 并把 opaque coordinator 注入 `NativeInferenceClient`。省略时 conversation requests 必须 fail-closed，不得使用 Python `ConversationCoordinator` |

### Native Conversation Owner Schema

当需要 FULL_CONTEXT/APPEND_DELTA continuation 时，顶层 requester config 可增加
`conversation` 对象。它必须完整包含以下结构；Python 只转发 JSON，不能读取或派生 key
bytes，也不能创建第二份 journal。

```json
{
  "schema": "ndnsf-di-native-conversation-v1",
  "journal": {
    "state_root": "state/conversation",
    "identity": "requester-a",
    "keys": [{"id": "active", "file": "keys/conversation.key"}],
    "quota_bytes": 67108864,
    "test_only_allow_ephemeral_state_root": false
  },
  "owner": {
    "requester_identity": "/example/user",
    "service_name": "/example/service",
    "security_domain_digest": "sha256:<64 lowercase hex characters>"
  }
}
```

`state_root` and key `file` paths are relative to `requester.json` and may be absolute only
when explicitly operator-owned. Each key file must be a regular file owned by the current user,
mode `0600` (no group/other bits), and contain exactly 32 bytes. `journal.identity` is a
single path component used for the journal directory and key derivation; it is distinct from
the NDN `owner.requester_identity`. `owner.requester_identity` must equal the configured
ServiceUser identity, `service_name` must be an absolute service name, and the security digest
must pass the native coordinator's exact digest validation. The native journal enforces its own
0700 directory/0600 file, writer lease, quota, encryption and restore rules. Volatile roots are
accepted only with the explicit test flag and are never a production qualification result.

requester 与 Core AA 的身份及证书须已在 PIB 中；CLI 不创建身份。grant 两个身份/签名
密钥独立，当前私钥为无交互读取的 Ed25519 PEM；encrypted PEM 不触发终端口令提示。
recipient_public_key_files 的键是 Provider NDN identity，值是其 grant recipient 公钥
文件；该注册表与 ACK 内用于 Core group wrapping 的 RSA key offer 是不同用途。
content_key_file 是已拥有的模型内容密钥，不是 CLI 新生成的随机 key。

## Catalog Schema

schema=`ndnsf-di-native-request-catalog-v1`，由 `NativeRequestCatalog::load` 消费：

| Field | Contract |
| --- | --- |
| model | 完整 SDK ModelDescriptor JSON；含 adapter 子对象和 source_revision 字符串；不接受未知或有损字段。字段权威为 NativeModelDescriptor::canonicalJson；输入方向对照 tests/fixtures/spec182/model-descriptor-oracle.json |
| source | file、data_name、digest、model_manifest_digest、canonical_graph_digest；可选 initializer_file 与 initializer_digest 成对出现 |
| recipe | artifact_profile_digest、assembler_descriptor_digest、backend_abi、precision、quantization、layout、padding、protection_epoch、max_source_bytes、max_assembled_bytes、max_nodes |
| publication | artifact_root；可选 package_manifest_digest（须等于 source.model_manifest_digest，省略时使用它）、layer_manifest_digests |
| input_format | OPAQUE 或 JSON，对应已有 catalog adapter 编码边界 |
| max_payload_bytes | adapter 接受的 payload 大小上限 |
| node_mapping | semantic node ID→实际 ONNX node indices；语义图与 ONNX 不一致时必须提供正确映射 |
| state_inputs / state_outputs | role→state family→实际 source tensor names；由既有 state binder 核对真实类型/shape，不能用语义名字猜测 ONNX tensor |
| splitter | 下列两种现有 native splitter 配置之一 |

QWEN splitter：kind=`QWEN`，layer_ranges、artifact_digests_by_role、weight_bytes_by_role、
roles、tensor_degrees。由 NativeQwenLayerSplit.inspectGraph 从固定 metadata 建立语义图；
node/state mapping 再绑定实际 ONNX 源。不是通过预制 plan 绕过 enumerate。

YOLO splitter：kind=`YOLO`、components、可选 postprocessing。每项 component 包含
candidate_id、priority、roles、node_names_by_role、input_ingress_role、result_egress_role、
merge_kind、candidate_digest、semantic_partition。复用 NativeYoloComponentSplit.fromOnnxCatalog
校验实际源及注册的语义分区。

## Minimal Layout Example

以下仅示例目录组织，不包含虚构可运行的模型/密钥配置。requester.json 中 catalog.model
须填写该模型完整的固定描述符，不能留下空对象或换用另一模型的 fixture。

```text
deployment/
  requester.json
  model/source.onnx
  model/initializers.bin          # only when the pinned source uses it
  policy/trust-schema.conf
  keys/requester-signing.pem
  keys/authority-signing.pem
  keys/provider-recipient-public.pem
  keys/provider-offer-public.pem
  keys/model-content.key
  input/application-input.bin
```

## Publication Authorization

发布会生成新 manifest hash。issuer 的 immutable publicationSources 固定原允许 manifest
对应的 model name/content、canonical source、initializer object 和 artifact profile。
新请求的签名绑定新 manifest hash，issuer 核对实际 publication bytes 与这些源身份后，
才使用原模型 key 签发新 manifest 的 grant。不因请求到来扩充 allowlist。

该配置入口支持本地已固定源；它不认证来自任意远程 URL 的配置。当前 CLI 输入为 INLINE；
REPO_REF 取数、完整 generation/feedback/conversation 验收继续由未完成任务负责，不能因
配置/请求 wire 支持某字段就宣称端到端能力完成。conversation owner 的配置接线只证明
native journal/coordinator 被 C++ 构造并注入；真实 Provider receipt/control、跨进程两轮
请求、恢复与 replacement 仍属于 T011-C/T016 的验收边界。

## Validation Ownership

R3-B1 负责配置读取、native library 接线、CLI build/help/error 及 unit 组合检查；R5-B5
负责 CLI 与 shared runtime parser 的组合边界；T016
负责真实 Core/Provider 与 MiniNDN 请求、失败/取消的验收。所有实际结果写入
[R3-B1 evidence](../evidence/r3-b1-request-lifecycle-20260908.md)。静态配置说明不能替代它。
