# T006 Real Qwen3 MiniNDN Evidence

Status: PASS.

Gate B in `results/spec165-local-gates/20260731T052926Z-5debf140` ran the
pinned local `Qwen/Qwen3-0.6B` ONNX graph through real MiniNDN with one User,
one Controller/security carrier, and three Provider stages. It retained two
warmups and six measured invocations over two prompts.

All eight invocation records are `OK`; each contains exactly eight generated
tokens, decoded answer text, TTFT, seven inter-token latencies, total latency,
tokens/s, and eight token-step lineage records. This is correctness evidence,
not a statistically strong performance comparison.
