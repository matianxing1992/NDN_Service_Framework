# T008 Core Artifact Publisher

## Scope and Source Review

基线 `4a25f67a`。实现 NativeCanonicalArtifactPublisher，可直接作为现有 preparation
ArtifactPort 使用。保持原 Core/DI 边界：ServiceUser::prepareServiceRequest、
publishEncryptedLargeData、postToIo/isOnIoThread 负责原生发布与线程调度，DI 不另起
Face、密码或分段实现。SourcePort 仍须由原生模型/目录 owner 配置；整个 requester
仍未接线，不把新增端口当成公开 request 完成。

实际源字节先核对 inspection object digest/大小，再复用 canonicalOnnxSourceIdentity
核对既有 recipe 的图和规范化 initializer 身份。新增二进制 digest 重载复用原 SHA-256
实现，避免将大向量复制为 string；异步工作共享 const 源对象。稳定 artifact 名用
ndn::Name 组合 candidate/logical role/rank，与加密 root fetch 名分开。

源码审查覆盖 Core prepare/publish/post、DI publisher/preparation、ONNX 原生身份及
现有 extraction-vectors。等待线程禁止为 Core I/O 线程；排队作业在 deadline/cancel
后丢弃工作及源引用，迟到 callback 不再发布；运行中的 Core 调用不强行中断，在返回
后停止下一个对象。只接受 success/encrypted、正确明文摘要/大小、scope、epoch 与
transport manifest 的 Core 结果；业务 root 始终另算 payload 摘要。

检测路径：冻结 SDK ONNX inline/external 字节 → 实际 native identity → publisher →
preparation binding/recertification；源篡改、规范化身份替换、错误 Core receipt、排队
超时/取消释放、source 发布后取消。另以 DummyClientFace 和已有 LocalMock wrapped-key
测试入口调用真实 Core API，要求实际加密签名/IMS 缓存成功，不接受失败后提前 return。
该 key fixture 不验证 NAC-ABE bootstrap、NDN 网络、权限资格或完整用户请求。

## Validation Plan

现有类型布局和旧 digest 签名保持，新增 publisher 类型和 binary digest 重载；复用
上一轮封闭工具链的 `.codex-tmp/spec182-t008-publication-recertification-r1/build`，
单一 -j4 必要增量构建 unit-tests。原始日志写入
`.codex-tmp/spec182-t008-core-publisher-r1/`，失败重试使用新目录。
只运行 publisher/preparation、planning/V3 placement/sealer、grant/client 相关单元。
不启动全量集成、MiniNDN、SIF 或 Tiger；公开 Python 扩展不作为此次 native 证据。

## Result

PASS for this focused unit。必要 -j4 build PASS（57.737s），focused exit 0，
64/64 cases、777/777 assertions PASS；新 Spec182CanonicalPublisher 为 6/6 cases、
67/67 assertions。真实 Core API 的 LocalMock-key 用例严格完成 4 个断言，未跳过。
它经过现有 Core 加密/签名/IMS 路径，检查 I/O 等待拒绝、缓存存在、缓存内容非原始
明文 root 和独立业务 manifest；不把这些断言扩大为密码或网络资格。

排队取消/超时用例证明源对象释放、迟到执行没有发布；source 发布后取消用例证明
没有 initializer/root 后续发布。inline/external 源身份沿用冻结 Python/ONNX recipe，
没有用 publisher 生成自己的期望 canonical digest。ldd.log 无缺失依赖，
vmstat start/end 后续采样无持续 swap-out，仅为短样本。
design-validation.json 与 git diff --check 的最终结果在 checkpoint 前核对。

提交前源码复核补充 Core 失败原因保留：success=false 时报告原 Core errorMessage，
与 malformed receipt 区分，防止实际 NAC/发布失败被折叠成泛化绑定错误。增加具名
负例后使用独立 r2 日志重新构建/测试；r1 PASS 不覆盖该最终补充。

最终 r2 build PASS（32.313s），focused exit 0，65/65 cases、779/779 assertions PASS；
publisher 7/7 cases、69/69 assertions PASS。失败原因保留的负例和既有六项均通过。
r2 design-validation.json errors=[]，git diff --check PASS。只改取消 predicate 的
线程/寿命注释，没有改变现有 DTO 布局或旧接口 ABI。

```bash
PATH=/usr/bin:/bin:/usr/sbin:/sbin:/home/tianxing/.local/node-v22.23.1/bin \
  python3 ./waf -o .codex-tmp/spec182-t008-publication-recertification-r1/build \
  build --targets=unit-tests -j4 -v
timeout 60s .codex-tmp/spec182-t008-publication-recertification-r1/build/unit-tests \
  --run_test=Spec182CanonicalPublisher,Spec182V3Placement,Spec182Preparation,Spec182OfferAdmission,Spec182NativePlanning,Spec182PlanSealer,Spec182GrantClient,Spec182NativeInferenceClient \
  --report_level=detailed --log_level=message
```

T008-A 保持 PARTIAL。SourcePort 仍待实际目录/本地 package owner 配置，inspection 的
实际 source 名、模型 revision 与 canonical node mapping 也须由该 owner 解析；当前
NativeInferenceClient 默认 request 仍停在 NATIVE_REQUEST_PIPELINE_NOT_READY。
下一步接本地 source inspection，再将 publisher/preparation 放入 Core ACK 后的真实
requester 流程。Python 扩展、完整请求、NAC-ABE bootstrap 和网络资格均未验收。
