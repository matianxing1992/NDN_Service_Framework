# R5-B6A Native Generation Stream Conversation Option Binding

## Status

`STATIC_PASS; BUILD_PASS; FOCUSED_BEHAVIOR_PASS; DONE` for this bounded binding exit.
T012-A remains `PARTIAL` because callback lifetime, complete parity, maintained caller migration,
cross-process delivery and T016 qualification are separate exits.

## Static review

按官方 `$review-agent` 的只读 defect-first 方法检查完整 diff、调用方和现有 native owner：

- `bindDistributedInference` 仍是唯一 Python DI translation-unit entry；没有新增 planner、
  subprocess 或 Python strategy trampoline。
- generation、conversation、ControllerVersion 和 Core stream options 直接映射已有 C++
  public DTO；`NativeRequestOptions` 只增加 optional nested values。
- `eventKeyGrant` 不把 `ndn::Block` 暴露给 Python，而是用解析过的 wire bytes/`None` property
  转换；native `onGenerationEvent` callback 保持 C++-only，避免跨线程 GIL/lifetime 误用。
- stream `validate()`、`wireEncode()`、`wireDecode()` 仍由 Core 执行；Python 只负责值对象
  构造和 bytes 边界。

静态复核未发现可执行的新缺陷。真实请求、callback destroy/observer 竞态、maintained
caller migration 和 Provider 运行资格不属于本批完成范围。

## Verification

源码语法检查（候选 NAC-ABE headers、system-first compiler）通过：

```text
PATH=/usr/bin:/bin:/usr/sbin:/sbin /usr/bin/g++ -std=c++17 -fsyntax-only \
  $(/usr/bin/python3 -m pybind11 --includes) -I. -Indn-service-framework \
  -I/usr/local/include \
  -I/home/tianxing/NDN/nac-abe-integration-182/install/include \
  -I/home/tianxing/NDN/nac-abe-integration-182/install/include/nac-abe \
  -I.local-boost171/include/nac-abe \
  -INDNSF-DistributedInference/cpp -IpythonWrapper/src \
  pythonWrapper/src/ndnsf/di_bindings.cpp
-> exit 0
```

候选 DI shared library 先按正确 Waf lock/output 重建，再构建 extension：

```text
WAFLOCK=.lock-waf PATH=/usr/bin:/bin:/usr/sbin:/sbin \
  CXX=/usr/bin/g++ CC=/usr/bin/gcc \
  ./waf build --targets=ndnsf-distributed-inference -j4 -v
-> exit 0; 5m38.247s

NDNSF_LIBRARY_DIR=/home/tianxing/NDN/ndn-service-framework/build-nac182 \
NDNSF_NAC_ABE_PREFIX=/home/tianxing/NDN/nac-abe-integration-182/install \
  PATH=/usr/bin:/bin:/usr/sbin:/sbin CXX=/usr/bin/g++ CC=/usr/bin/gcc \
  python3 setup.py build_ext --inplace --force --parallel 4
-> exit 0; extension build log retained in the local run directory
```

The first extension import failed at the source-closure boundary because the old candidate
`libndnsf-distributed-inference.so` did not export
`NativeAdapterDescriptor::descriptorDigest()`. A preceding Waf command also selected the stale
`.codex-tmp/spec182-r4-b2/build` lock because `--out` was placed after the `build` command. Both
failures were retained under `.codex-tmp/spec182-r5-b6a-native-options-20260908/` and corrected
before final verification; no source rollback or fallback was used.

```text
PYTHONPATH=/home/tianxing/NDN/ndn-service-framework/pythonWrapper \
  /usr/bin/python3 -m pytest -q tests/python/test_spec182_native_bindings.py
-> 10 passed in 0.29s
```

`ldd` resolves `libnac-abe.so` to the explicit integration prefix and Core/DI libraries to
`build-nac182`; `nm` confirms the descriptor digest symbol. SHA-256 records and compiler/link
commands are retained in the same local run directory. The corrected build's `vmstat 1` showed
no sustained swap-out after its first sample; the existing swap allocation is not qualification
evidence.

## Coverage matrix

| Lane | Status | Evidence |
| --- | --- | --- |
| production entry / callers | covered for binding entry; gap for maintained callers | `bindDistributedInference`, `NativeRequestOptions`, existing requester remains R5-B6 |
| implementation and wire | covered | DTO fields, Core stream validation/wire round-trip, bytes conversion property |
| test / harness / oracle | covered for binding DTOs; gap for real request parity | 10-case `test_spec182_native_bindings.py`; existing C++ generation/stream/conversation selectors remain dependencies |
| build / source closure | covered for local candidate | explicit DI rebuild, explicit NAC-ABE prefix, extension RPATH, `ldd`/`nm`/hash records |
| migration / evidence | covered for this bounded exit; gap for full migration | durable evidence and local run logs; R5-B6/T012-A/T013/T016 remain open |

## Remaining boundary

The binding now provides a stable value-object exit for the next migration batch. It does not
claim Qwen or streaming requester migration, native callback lifetime qualification, real
Core/Provider parity, legacy runtime retirement, or cross-process/Tiger qualification.
