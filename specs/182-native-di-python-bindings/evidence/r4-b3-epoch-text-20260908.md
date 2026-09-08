# R4-B3 Epoch Text Commit Boundary

## Scope and Status

T011-B local batch / DONE。复用 NativeEpochCoordinator 与真实 NativeTokenizer，
先判断 EOS/MAX/stop，再执行 terminal stable flush，之后才接受事件、发布反馈并提交状态。
本批不关闭会话日志、真实网络或 T016 验收。

## Members and Validation

- ET-1：修正终止文本刷新顺序；只读静态检查前缀、终止判定及副作用顺序。
- ET-2：C++ 真实 tokenizer/epoch 回归覆盖 MAX、EOS、stop、重算前缀和 decoder mismatch。
- 批末复用 R4-B2 构建树增量构建 unit-tests，运行新增及相关 epoch/stream 测试。
  未运行测试前保持 PARTIAL，不按静态检查授予行为 PASS。

## Initial Finding

当前源码对终止 token 使用 stableTextDecoder(ids, false)，事件接受和状态提交后才
调用 final=true。ByteFallback 开放字节段因此可能只出现在 final payload 而不在事件中；
final decoder mismatch 也发现得过晚。修改只移动文本决定边界，保持事件→反馈→状态顺序。

## Static Review

ET-1、ET-2 和批次流程按已安装 review-agent 只读审查：No findings。
完整 decode 仍用于 stop 判断；terminal flush 和 prefix 校验先于 eventSink；后续
feedback/state 提交顺序未变；重算 prefix 不重发事件。测试直接调用真实 coordinator，
MAX/EOS/stop/replay 使用已冻结 ByteFallback tokenizer，另检查 final mismatch 零事件。
此次未新增 ABI，复用 R4-B2 系统编译器/Boost 1.71 构建树，按既有内存降档 -j2。
这些检查尚不证明运行成功，也不覆盖 feedback/state 故障注入。

## Build Attempt

初次增量 build exit1：测试 parse optional finalPayload 类型错误，静态审查未发现；
原始日志 [.codex-tmp](../../../.codex-tmp/spec182-r4-b3/build.log)。补必需存在性断言后
解引用；生产源码已编译，尚无运行结果。下一次复用同树并保留独立 r2 日志。

## Final Local Result

修复后同树增量 unit build PASS / 24.266s；实际 ndnsf-distributed-inference 共享库
也增量构建 PASS。未重建 Core/UAV 或创建 fresh tree；保持原功能构建配置，非发布性能资格。
命令与原始日志位于 [.codex-tmp/spec182-r4-b3-r2](../../../.codex-tmp/spec182-r4-b3-r2/)：

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin /usr/bin/python3 ./waf -o .codex-tmp/spec182-r4-b2/build build --targets=unit-tests -j2 -v
PATH=/usr/bin:/bin:/usr/sbin:/sbin /usr/bin/python3 ./waf -o .codex-tmp/spec182-r4-b2/build build --targets=ndnsf-distributed-inference -j2 -v
LD_LIBRARY_PATH=/home/tianxing/NDN/nac-abe-integration-182/install/lib:/home/tianxing/NDN/ndn-svs/build:/usr/local/lib:/opt/onnxruntime/lib:.codex-tmp/spec182-t001-dependencies/onnx-install/lib timeout 180s .codex-tmp/spec182-r4-b2/build/unit-tests --run_test=Spec182EpochText,Spec182StreamAcceptance,Spec182Sampling,NativeEpochCoordinator* --report_level=detailed --log_level=message
```

上述测试 exit0，24 cases / 411 assertions PASS；新增 Spec182EpochText 2 cases / 28
assertions。真实冻结 ByteFallback 在 MAX/EOS/stop/replay 四路径中事件拼接等于 final，
final decoder mismatch 在零事件接受处拒绝。既有 11 个 NativeEpochCoordinator cases
覆盖取消/deadline、拒绝事件、反馈发布失败回滚、Provider state 与终止反馈。
同次 requester stream 7 cases / 188 assertions、sampling 4 cases / 40 assertions PASS；
记录本次实际断言数，不沿用旧运行计数。

Context Mode active health exit4（plan/tasks indexed hash stale），本轮使用仓库权威记录；
CodeGraph 已定位 canonical coordinator，源码直接核对。未修索引或使用 timeline 推断进度。
T011-B 的全部生产接线核对、T011-C 会话日志与真实恢复、T016 integration 仍保留，
本批 DONE 仅指终止文本修复和上述本地回归，不关闭父任务。
