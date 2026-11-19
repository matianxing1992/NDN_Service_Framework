# Experiment Contract — Targeted vs Basic

This is the single parameter/statistics authority. Campaign-02 remains archived; its late-joining first calls contain accumulated publication replay. The revised clean-history campaign below requires new measurement, without reusing campaign-02 results as its qualification.

## Questions and Arms

Q1：完整目标prefix是否保证仅该设备执行？Q2：eligiblefast是否少前台ACK/Selection？Q3：计入bootstrap/refill/失败后实际等待减少多少？

| Arm | Initial condition and meaning |
| --- | --- |
| BASIC | 已知同一目标、basic RequestService显式单Provider、FirstResponding；当前权限就绪 |
| TARGETED_BOOTSTRAP | 同目标权限就绪，fresh调用owner没有cached targeted material；实际bootstrap路径；不是机器开机时间 |
| TARGETED_STEADY | 同目标、已完成bootstrap/准备，普通运行包含fast、refill、fallback与后台流量 |
| FAST_SUBSET | STEADY中actualMode/前台count证实fast的条件子集，只作机制说明，不替代STEADY成本 |

fresh owner首调用在缺少实际mode证据时只能标TARGETED_FIRST_CALL；有现有callback/trace等证据确认bootstrap后才标TARGETED_BOOTSTRAP。可在模拟Provider回调记录公开RequestMessage.getRequestMode，不需要改框架。必需的是保留独立首调用样本，不能因细分缺测伪造bootstrap标签或丢掉样本；论文对应标首调用而非已验证bootstrap。

保持tokens enabled。若有效security profile让fast不适用，则报告ineligible路径，不能关闭安全补齐收益。模拟Provider注册同一NormalAndTargeted无物理动作echo handler，通过现有公开API调用；实验不使用UAV-APP、不修改产品代码。UAV仅论文用途示例。

## Required Matrix

- 主机制网络：有线MiniNDN，gs→relay→drone1；Controller在gs，同样的NFD/FIB/SVS；对称两条链路，1Gbps，loss=0；每链路单向delay d∈{1,10,50}ms。轴同时报告配置d和实测baseline path RTT，不将配置直接当应用RTT。
- 每个d，BASIC与TARGETED_STEADY做5个独立paired blocks，paired order按预存种子{20301,20302,20303,20304,20305}随机/交替平衡。每arm独立client状态，相同Provider handler/security及可比准备条件。
- 每arm readiness后warmup10s，正式低负载closed-loop恰好60s提交窗口，随后bounded drain，且恰有一个outstanding；handler delay=5ms，请求/回显payload=64byte（含唯一commandId），每attempt deadline=5s，下一attempt不因晚响应重排。实测duration、完成数、实际rate公开，不能把closed-loop吞吐称相同offeredRPS。
- 同一d/每block额外10个独立TARGETED_BOOTSTRAP尝试：每个sample使用独立新ServiceUser owner/进程，同一个预授权完整User身份、证书及权限；每arm/sample在独立fresh Sync group中启动同身份的两个Provider并等待真实权限/解密就绪，再启动User。Provider就绪及owner启动时间均在API计时外，磁盘cache保留；不得恢复private token pool。不同arm/sample不得继承业务publication history。每attempt有独立终态，owner stop/wait后停止并等待该group的Provider；报告50个样本/每d和独立block汇总。bootstrap不是steady60s测试，不强套steady warmup。该条件评估clean-history fresh-User invocation，而不是机器开机或late-join catch-up。
- 必需鲁棒性单元：d=10ms、per-link loss=1%，BASIC/STEADY各5paired blocks，同样60s/window/security。用于报告可靠性与尾延迟，不要求targeted超过basic。
- 必需目标隔离单元：至少两个模拟Provider target1/target2同服务，正常及target1离线，通过真实MiniNDN Response身份和实验自有handler记录验证target2 wrong-target executions=0；故障期间不自动切换。不调用UAV-APP，不增加生产test fixture。
- steady真实维护开销不能剔除，复用existing日志/callback/capture核对；若默认窗口没有refill，可用已有batch配置预先声明独立maintenance case，不改框架/私有pool。revoke/version/replay/expired/restart全矩阵不是203前置，另行研究不阻塞本调用方式比较。
- smoke ≤10s只证明接线。正式campaign预计60–90min，启动前冻结最大2h执行预算；超时保留PARTIAL，不改变deadline/seed/样本数后补成同候选PASS。

## Metrics and Fairness

**Measurement cohort**: readiness/warmup结束后客户端单调时钟t0，t1=t0+60s；submit∈[t0,t1)的attempt构成唯一主cohort。t1停止新提交，对此cohort drain到全终态，最晚到t1+5s deadline+2s shutdown margin；缺终态或超界cleanup使run INVALID，仍保留raw。跨窗terminal保留在原cohort，分母不变；cohort successful completions/60s称cohort completion rate，另列仅terminal∈[t0,t1)的in-window completion rate，不能用drain时长悄悄换分母。cold按其独立sample cohort统计。
后台维护有两种并列scope：按requestId/trigger关联的cohort work（cohort触发的窗后refill也计入，追踪到child terminal）和[t0,t1)时间窗wire（包括warmup触发但窗内发生的refill）。readiness、warmup、measurement、drain流量分别列；无法关联的maintenance保留到run总量并标unattributed，不静默丢弃。CI比较使用同一声明scope。

权威latency=客户端API submit到validated terminal，client monotonic；timeout/submit/error独立记录。成功样本latency分布必须和success/timeout/error率并列；timeout不是0ms，失败不能消失。late/duplicate不修改终态，不污染另一请求。
报告每run attempts/success/error/timeout、successful p50/p95、success ratio、实际完成rate；现有观察能关联时报告late/duplicate、前台与维护message/逻辑bytes，不能观察时NOT_MEASURED而非0。必需wire capture为d=10ms/loss=0的BASIC/STEADY完整5paired blocks：固定relay→模拟target1链路单一端点interface双向capture，避免两端重复计数，报告measurement/warmup/drain维护在内的NDN transport总wire bytes，能关联再细分。capture filter、丢包计数、接口/时钟/scope写manifest；缺capture或capture丢包使SC004 PARTIAL。其余单元网络wire可NOT_MEASURED；不能以逻辑Data size代替实际wire，也不称全拓扑总bytes。探测RTT流量单独tag/剔除业务计数。
报告User生命周期、Provider handler工作span、请求/ACK/Selection/响应/维护事件和network/retrieval等待的可辨部分。只用同clock或已校准clock作差；不能简单累计重叠Provider等待；其余为mixed/unattributed。实测基线使用同一路径的独立小Data探测、相同Provider签名身份和信任校验、非cached唯一名称，注明Responder。称“签名小Data往返”，包含签名/校验时间；不包含业务NAC/Controller授权处理，不能解释成纯链路RTT或与完整业务保护成本相同。
统计单位为5个独立paired blocks：每pair计算p50/p95/成功率差，报告pair分布和固定seed=20399、10000次pair-level bootstrap的95% CI。少样本/degenerate CI说明；单run少于100个success的p95标`LOW_SAMPLE_COUNT`，不隐藏样本或延长某一arm窗以选择结果。不得以每个request为独立重复获得虚假显著性。
高负载rate10/30/70RPS、WiFi mobility及gRPC/NSC新对比为optional独立扩展，不阻塞203、不拼入主结论。

## Qualification and Failure Boundaries

入口必须证明actualtarget、tokens enabled、currentpermission和Response结果/身份。Sync multicast/其余Data best-route最长匹配及FIB逐node读取。细mode/count缺existing证据时仅限制FAST_SUBSET或阶段结论，不为此改Core，也不抹掉真实API总latency；错目标/结果、缺终态使runINVALID，实际业务timeout所在有效run保留统计。
必需capture单元必须满足上面的接口/无drop/scope条件；非capture单元的NOT_MEASURED不替代必需capture资格。
保存source/dirty patch、installedbinary/lib/header、protocol/launcher/analyzer/oracle、security配置、topology、contract digest及随机序列。paired mismatch pre-dispatch拒绝零业务traffic。
原始log/manifest/packet evidence归档在local specs evidence，cache和workspace分离；零跨候选结果继承。stop/wait全部children后再停NFD，cleanup失败不是完整资格PASS。

## Manuscript Outputs

### T005 four-arm extension (user authorized)

Run20 new matched blocks: d1/loss0,d10/loss0,d50/loss0,d10/loss1, seeds20301–20305. Every block runs NDNSF basic,targeted,gRPC,NSC in cyclic rotations of that order, offset(seed-20301) mod4, with no parity reversal. The first4 blocks give each arm each position once; the fifth repeats the first order, so position counts are1 or2. The analyzer must check every actual order. Same gs-relay-drone1 two-hop1Gbps executor path,64-byte unique echoed commands,configured5ms handler,10s warmup/60s submission cohort,one outstanding request,5s success deadline,window crossing terminals drained7s and owner shutdown after worker completion. NDNSF basic and targeted retain the same two participating providers; gRPC and NSC address the fixed drone1 executor directly and do not implement NDNSF discovery. This compares complete invocation modes, not equal discovery or security primitives. Permission/identity/channel readiness outside measurement; no first-call samples in main four-arm plot. Whole budget3h. Pilot/smoke is not performance; missing groups fail qualification. Campaign05 was stopped for a structural order-position confound and is descriptive-only; the corrected independently frozen campaign06 is the sole T005 qualification cohort, with no pooling.

Reuse NDNSF actors unchanged. NSC uses existing signed notify→input fetch→result fetch actors, experiment-only serial window/echo/native JSONL mode; legacy defaults preserved. gRPC uses existing Greeter proto/schema and a small echo adapter with TLS server/client certificate verification; allowlisted client/provider identities. NDN request/content authentication and NDNSF ABE/request-scoped confidentiality remain enabled. NSC signatures do not provide payload confidentiality; TLS and ABE are distinct mechanisms. Four groups compare the stated deployed configurations, not security-equivalent primitives or isolated protocol cost. No downgrade of NDNSF, synthetic protocol delays, substitute targets, or old-gRPC/NSC values.

Each success requires validated64-byte exact echo and target identity plus one independently recorded handler execution. Report p50/p95,all attempts/success/errors/timeouts,rates,paired block CIs. Freeze binaries/libraries/native+Python sources/security material hash and topology settings before campaign. Reuse stable work root; signed NDN originals/names remain untouched. Read-only source review and real MiniNDN native oracle precede formal run. Required negative checks: wrong payload/identity rejected,deadline/cleanup retained. Runtime profile none for unchanged production targets; native experiment actors need independent callback/ownership review and bounded smoke, not a claim of sanitized product qualification.

### T004 independent wire supplement

Campaign03 is the frozen 20-cell primary latency cohort. Its UDP-port-filtered captures omit noninitial IPv4 fragments and are PARTIAL for traffic; retain every native attempt and do not alter raw evidence. Separately freeze five d10/loss0 paired wire cells, seeds 20301–20305, alternating order 3:2, 10s warmup, 60s measurement, within 1800s. Keep native binaries, libraries, headers, C++ sources, wscript, identities, trust, permissions, topology, deadlines, payload and configured handler delay identical. Only capture/postprocessing instrumentation changes; skip subsequent cold samples. Never pool supplemental requests into primary latency or first-call statistics.

Capture full IPv4 on the same relay–target interface with 250ms guards before User readiness and after terminal drain. Postfilter UDP6363 only after complete fragment association by addresses/protocol/ID/time. Reject missing ranges, truncation and conflicting overlaps. Count duplicate physical frames while deduplicating identical bytes for reconstruction. Record zero kernel drops and capture start/stop coverage. Report attempts, successes, errors and timeouts separately, total/window frame bytes, window bytes per submitted attempt and per second. These normalized quantities include background traffic and are not exact per-request attribution. Full acceptance requires 20 native primary cells, 150 independent first calls, target isolation, five complete supplemental capture pairs and execution-identity compatibility; preserve both frozen identities.

按用户最新要求，主图只比较NDNSF basic/targeted steady；first-call作为独立初始化说明保留文字，不能当作第三个系统或混入稳态图。主表报告success/attempt、paired CI，阶段耗时与独立capture开销用文字报告。跨系统目标为NDNSF basic、NDNSF targeted、gRPC、NSC四组，但必须同拓扑、工作负载、安全配置和计时边界重测，旧不同条件数据不得拼接。当前四组匹配证据尚未生成，现有图明确标within-NDNSF。保留原始及完整派生证据；FAST_SUBSET仅在充分证据时报告，否则标缺测。系统启动不纳入主比较。
文字可写“指定目标”“有效缓存材料允许跳过前台ACK/Selection”，实测无收益时照报。不能写普适1RTT/零安全成本/保证飞行控制deadline/真实drone飞行完成/targeted改变NFD传播方式。
