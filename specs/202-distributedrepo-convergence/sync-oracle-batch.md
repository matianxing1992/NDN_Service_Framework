# T006 Signed Sync Mapping Oracle

**Batch growth decision**: 独立于T002 nativebuild；只修改offlinepacketoracle、A21两阶段captureoffset与finalizedcapture接线、Python offline negative fixtures。不改Repo/Core/SVS/RPC，不运行MiniNDN。
**Status**: IMPLEMENTED / STATIC_PENDING，behavior/当前network NOT_RUN。fixedreview ZIP`evidence/t006-static-review-20261008/review-snapshot.zip` SHA`a455a14c274b1346e4960ae1bb12f644e1404dc022144d268a3036a425512aec`。
**Actual input proof**: olda21-diag-002 caller publiccert绑定consumer；caller签名SyncInterest Data含MappingData205，nativeentry字段210→212/214与Name7，嵌套205支持其他producer；STATUS逻辑seq1/2与6/7/8已静态离线核对，不能算新candidate runtime资格。
**Oracle**: Wireshark3.2不支持udp.payload，使用data.data+ip.reassembled.data，ip.defragment:TRUE；完整Interest5→AP36→Data6，ParametersSha256Digest+RSA/KeyLocator/group/sourcecertificate链验证。producer/bootstrap/seq去重，entrywire hash冲突拒绝，first-seen frame区分新的offlinepublication和缓存piggyback历史mapping；caller/Controller三条相同bytes/order，native三timeout只作ordinal Provider归属推断。
**Test/canonical fields**: test_spec202_sync_oracle.py用ndn.encoding.make_data/make_interest和临时内存RSA key生成publicwire；test TLV helper字段/顺序来自实际pinned SVS映射和已解码wire（205/7/206/210/212/214/7），不是产品状态模拟。负例覆盖paramsdigest、重新计算digest后坏RSA、unpinnedKeyLocator不计caller证据、group、sequence形状/嵌套界限、truncatedwire；conflict/retransmission与证书链的closing覆盖仍需静态review确认。
**Dynamic profile**: none，test-onlyofflinePython；密码学验签和真实packet独立oracle仍必须通过，不用none省略行为。native业务/Provider metadata安全仍T005/T009/T010。
**Budget/evidence**: offlineunit≤60s；oldpcap caller+Controller≤120s，各freshreport；最多一次changedgate后retest，不伪装二轮network。当前正式A21只在T009 convergence后运行。
**Five lanes**: entry为finalpacketverification A21 branch而非scenario dispatch；implementation为strictTLV/signature/IP parser与mappingidentity；fixture为canonicalwire/negative和phasedfirstseen；build/source为source_candidate_files注册与Pythonimport（T009须显式补newtest/202合同）；migration为旧packet证据单列，新capture增加offset不改变原nativeattempts/receipt/destination与cleanup。

## V2 Static Release

初审v1为BLOCK，三个P2分别为producer误用caller identity、缺distinctRequest约束、未核对expectedProviderorder。现用NDN Name component检查caller/user/numeric-session；要求同一producer及三个不同Request；runner传expectedC/B/A。增加wholecapture retrans/firstseen、conflictingmapping、duplicateRequest、wrongidentity/wrongorder、两capture不同/缺条目、badcallerchain、missingcaller/root反例。

完整v2 snapshot `evidence/t006-static-review-20261008/review-snapshot-v2-complete.zip` SHA`60be34ccc993077ef6049512fc4cbcdde3c1754c74f12f909b9b07ce16652124`只读五lane复审PASS，释放bounded offline test/reanalysis，尚无behavior/networkPASS。先前review-snapshot-v2.zip因错误contract路径失败为PARTIAL；新归档先验证全部输入存在，旧文件不作为authority。复审建议补同Provider集合错序[C,A,B]反例，已补。

协议限制：支持完整IP reassembly和完整NDN-LP Fragment；不重组多个NDN-LP fragment，不计不完整packet为publication。若实际case不能观察全部三个完整映射则fail closed，不把缺包记PASS。新test与202contract source登记由T009严格实际closure负责。

## Offline Closing

12 unittest PASS，offline-unit-3.log，3.286s。首次invocation将路径当tests.minindn包，ModuleNotFoundError；改用PYTHONPATH=tests/minindn与模块名。第二次setup发现本机旧cryptography的generate_private_key签名要求backend，显式default_backend()修复三个调用。Changed gate：核对已安装依赖inspect.signature和实际module/package位置；只读delta复审PASS，保留前两失败日志，不以重跑代替身份检查。

historical-mapping-reanalysis.log使用当前签名parser解析旧a21-diag-002两capture，caller/controller各6条STATUS，session508的seq1/2/3/6/7/8，跨capture对应映射一致。旧manifest缺本次新增offlineoffset，故不能声称当前A21顶层phase verification或新runtimePASS；它只是historical offline辅助证据。T006当前网络和source登记保持PARTIAL。
