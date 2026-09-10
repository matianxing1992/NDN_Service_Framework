# Spec183 host gate: APP v33

Updated 2026-09-10. The host gate rebuilt the native closure with
`spec183_host_build.sh` at maximum `-j4`, then verified import, the real
MiniNDN runner help path, `_ndnsf.so` and its `ldd` closure. The result is
`YOLO_HOST_GATE_COMPONENT_ONLY`, not a GPU or Tiger result.

| Item | Result |
| --- | --- |
| Receipt | `Experiments/TigerCluster/results/host-minindn-v33.json` |
| Schema | `tiger-yolo-host-minindn-manifest-v2` |
| Status | `PASS` |
| Receipt digest | `sha256:ed29dbe94d6016eea0a9ad8d658143c0a0f2796789b6b3b54b04f258dadb0282` |
| Framework | `sha256:27fb862f1d917ee304c3fc7ece767d6dfde631474fac3b43796718d6c689f3b5` |
| NDN-SVS | `sha256:0f88bec46de7e58dd182ce9dc0b4cde403d68896a3d76c731d4f26cbff9dcbdc` |
| NAC-ABE | `sha256:d9abccf290515e9bc782bfde12973f9ef1757f619ca650e70cccac37ec9cb9a5` |
| APP | v33, manifest `sha256:2df82daa7f5684ebda690fa325e054ca3d992da1d36a6b2fcbda54af343f2b42` |

The first host-gate invocation was rejected because a stale execution log
already existed (`HOST_GATE_EXECUTION_OUTPUT_EXISTS`). Removing only that
stale output and rerunning produced the retained PASS receipt.
