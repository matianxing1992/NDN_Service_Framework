# T003 — 跨语言 parity 向量锁定

> **Current scope correction (2026-09-05)**: T003 已完成定向验收：以下历史记录仅证明 grant 解包 parity；新增 8 个装配向量及双生产入口 16 项检查见 [装配验收](t003-assembly-parity-20260905.md)。本轮 grant/assembly 合计 19 PASS，整体资格仍等待 T007。
> 当前裁决与下一步以 [audit.md](../audit.md) 为准，以下保留为原始范围记录。

**Layer**: implemented（向量生成器 + 固定向量文件 + 双侧消费测试）;
executed（Python 侧 3 组 parity 测试全绿 + C++ 侧 3 用例全绿，
2026-09-05）;无 measured 声明。

Date: 2026-09-05. Source HEAD: commit `6408df8e`
（T003 vectors + parity tests）。

## 声称

同一 grant 字节流（规范 JSON）分别由 Python 与 native 解包得到同一
内容密钥;向量含正例与全部负例（错误收件人、跨请求/attempt/core/
model/纪元、过期、伪造签名）。向量文件随任何编码变更必须同步
更新并重新双侧验证。

## 资产

- `tests/fixtures/spec181/grant-vectors-v1.json`：schema
  `spec181-grant-vectors-v1`;固定密钥（确定性种子派生）、固定绑定
  （requestId/attempt/planCore/modelManifest/protectionEpoch/nowMs）、
  9 个 case（wire + 每 case binding/nowMs + 期望 {ok, contentKey,
  reason}）。
- `scripts/gen_spec181_grant_vectors.py`：再生入口（改动编码后
  MUST 重新生成）。
- Python 消费：`tests/python/test_spec181_native_grant_parity.py`
  （Python 侧与期望一致、native 侧与期望一致、双侧互一致）。
- C++ 消费：`tests/unit-tests/distributed-inference-native-grant-verifier.t.cpp`
  （逐用例断言 verified 与 contentKey）。

## 测试执行（executed）

```
python3 -m pytest tests/python/test_spec181_native_grant_parity.py -q
3 passed
./build-system-j2/unit-tests --run_test='*GrantVerifier*' --log_level=message
Running 3 test cases... *** No errors detected
```

正例内容密钥 `d6d8c81c...`（向量文件第 1 个 case）双侧一致。

## Verdict

PASS（T003 范围）。跨语言 parity 由固定向量双侧锁定。
