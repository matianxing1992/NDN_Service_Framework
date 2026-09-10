# R11-B8-G27 Routable IPv4 Address Boundary

**Date**: 2026-09-10
**Status**: `CLOSED_FOR_VALIDATION` for this bounded v1 topology address gate
**Parent**: R11-B8 / T014 / Spec110 allocation topology

## Finding and repair

`allocation_topology.py` 之前只调用 `ipaddress.ip_address`，因此 IPv6、loopback、unspecified
或 multicast 地址可以进入 process map；但 v1 NFD 配置和 route launcher 固定使用 IPv4
`tcp4://`/`udp4://`，而多机节点的 `127.0.0.1` 只在各自 network namespace 有效。该组合会让
MiniNDN 的“每节点 loopback”掩盖跨节点路由错误。

现在 v1 map 要求节点地址是 IPv4；拒绝 unspecified、multicast，并在 `multi-node` 拒绝
loopback 与 link-local 地址。route binding 仍由已有目标节点地址/selected transport 校验
负责，失败停在拓扑 preflight。

## Validation

- `python3 tests/container/itiger-qwen-live/unit/test_allocation_topology.py`：21/21 通过，
  新增 `127.0.0.1`、`0.0.0.0`、multicast 和 IPv6 反例。
- `bash tests/container/itiger-qwen-live/integration/test_network_scripts.sh`：
  `NETWORK_SCRIPT_PASS`。
- `python3 -m py_compile packaging/ndnsf-di-container/lib/allocation_topology.py`：通过。

该门只修正 v1 地址语义；不代表真实节点路由、端口分配、SIF/ELF、GPU、跨节点 NDN、
no-Python 或 T016/T017 资格。
