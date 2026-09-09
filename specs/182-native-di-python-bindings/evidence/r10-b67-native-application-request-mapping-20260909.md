# R10-B67 Native/Application Request Identity Mapping

## Scope

本批次冻结 R10-B65 F-05 的身份边界。native requester 仍由唯一 owner 分配
authoritative Core `requestId`；维护调用方已有的 `wire_request_id` 通过
`NativeRequestOptions.applicationRequestId` 作为非权威 correlation 传入，handle
同时暴露两层 ID。Qwen native helper 显式转发该 correlation，并在返回前核对映射。
本批次没有宣称跨进程请求、Provider worker、conversation continuation 或 T016 资格。

Baseline: `c8f44071` (R10-B66 local repair checkpoint).

## Implementation and static review

- `NativeRequestOptions.applicationRequestId` is bounded and rejects NUL bytes; it
  never replaces the owner-generated `/NDNSF/DI/REQUEST/N` Core identity.
- `NativeInferenceHandle::applicationRequestId()` returns the immutable caller
  correlation stored in the operation. The pybind surface exports
  `application_request_id` on options and handles.
- `APPClient.request_native_reference` accepts an optional `request_id` only as
  caller correlation and forwards it without enabling planner fallback.
- `_native_qwen_request` forwards `request_id`, returns that caller ID as its
  `request_id` for the maintained caller contract, and retains the native owner
  ID as `native_request_id`; a mismatch fails closed.
- Contract documents now state the two-layer mapping and preserve native owner
  authority.

The complete diff was checked for ownership, lock scope, ABI/source registration,
backward-compatible omitted arguments, and maintained caller reachability. No
introduced control defect was found by static review.

## Five-lane coverage

| Lane | Coverage | Boundary |
| --- | --- | --- |
| production entry/callers | covered locally | `APPClient.request_native_reference` → `NativeInferenceClient::request`; maintained `_native_qwen_request` propagation |
| implementation/wire | covered locally | operation owner ID remains Core transport identity; application ID is explicit local correlation; no envelope schema expansion |
| test/harness/oracle | focused behavior PASS | C++ handle mapping assertion; Python DTO, Qwen caller and public facade forwarding assertions |
| build/source closure | PASS | Waf `unit-tests` and `ndnsf-distributed-inference` rebuilt from the same source; pybind extension rebuilt against candidate libraries; `nm` exports handle mapping symbol |
| migration/evidence | PARTIAL | F-05 local contract closed; F-01 planner reachability, F-06 real process/worker path, caller retirement and T013–T017 remain open |

## Validation

```text
env PATH=/usr/bin:/bin:/usr/sbin:/sbin:$PATH CXX=/usr/bin/g++ CC=/usr/bin/gcc \
  ./waf -o build-nac182 build --targets=unit-tests -j4
  PASS; 56.496 s

env PATH=/usr/bin:/bin:/usr/sbin:/sbin:$PATH CXX=/usr/bin/g++ CC=/usr/bin/gcc \
  ./waf -o build-nac182 build --targets=ndnsf-distributed-inference -j2
  PASS; 43.736 s

NDNSF_LIBRARY_DIR=.codex-tmp/spec182-r4-b2/build \
  python3 setup.py build_ext --inplace --force --parallel 4
  PASS; pybind extension rebuilt

env LD_LIBRARY_PATH=.codex-tmp/spec182-r4-b2/build:/home/tianxing/NDN/nac-abe-integration-182/install/lib:/home/tianxing/NDN/ndn-svs/build \
  PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:pythonWrapper \
  python3 tests/python/test_spec182_native_bindings.py
  PASS; 16/16

env LD_LIBRARY_PATH=.codex-tmp/spec182-r4-b2/build:/home/tianxing/NDN/nac-abe-integration-182/install/lib:/home/tianxing/NDN/ndn-svs/build \
  PYTHONPATH=NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:pythonWrapper \
  python3 tests/python/test_ndnsf_di_app_sdk_compatibility.py
  PASS; 18/18

./.codex-tmp/spec182-r4-b2/build/unit-tests \
  --run_test='Spec182ClientState/ConfiguredClientClosesEmptyAckAndCancelsActualCorePendingCall'
  PASS

ldd pythonWrapper/ndnsf/_ndnsf*.so  # with explicit LD_LIBRARY_PATH
  candidate framework/DI, NAC-ABE integration prefix, and ndn-svs paths observed
nm -D --defined-only .codex-tmp/spec182-r4-b2/build/libndnsf-distributed-inference.so \
  | c++filt | rg 'NativeInferenceHandle::applicationRequestId'
  PASS; exported symbol observed

git diff --check
  PASS
```

An initial extension test attempt failed before test bodies because `/usr/local/lib`
provided an incompatible NAC-ABE library; after selecting the explicit integration
prefix, it failed because the shared DI library was stale and lacked the new symbol.
Both raw boundaries are indexed in `docs/failure-log.md` and retained under
`.codex-tmp/spec182-r10-b67-binding-boundary-20260909/`. No protocol result was
inferred from either loader failure.

## Closure decision

`CLOSED_FOR_VALIDATION` for the identity-mapping contract. `PARTIAL` remains the
correct Spec status: only a real native-config Qwen requester → Core → Provider
worker/process run can close F-06 and validate the mapping across the network.
