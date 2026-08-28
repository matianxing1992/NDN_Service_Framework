# T009 host Python extension refresh — 2026-08-23

Status: `PARTIAL_PASS`; current host ABI/import/linkage is verified, but real
Provider-to-User callback/iterator delivery still belongs to T009/G1.

## Subject

- Host Python: `3.8.10`
- SOABI: `cpython-38-x86_64-linux-gnu`
- Imported extension:
  `pythonWrapper/ndnsf/_ndnsf.cpython-38-x86_64-linux-gnu.so`
- Extension SHA-256:
  `f74a77c595ecc432f3d8778cfc5b1f6c358448f430c92765f4a1eb869dec3f06`
- Framework SHA-256:
  `b84d0d71c141252515d0d2a75ca077bfccfa43c70d0375fe4cace3a0bc8e7c98`
  for `build/libndn-service-framework.so.0.1.0`

## Commands and result

```bash
cd pythonWrapper
python3 setup.py build_ext --inplace

python3 -c 'import ndnsf._ndnsf as n; print(n.__file__)'
ldd pythonWrapper/ndnsf/_ndnsf.cpython-38-x86_64-linux-gnu.so

PYTHONPATH=pythonWrapper:NDNSF-DistributedRepo/pythonWrapper:NDNSF-DistributedInference:Experiments \
  python3 -m pytest -q --tb=short \
  tests/python/test_streamed_invocation_api.py \
  tests/python/test_ndnsf_python_service_response_binding.py \
  tests/python/test_spec168_provider_generation.py \
  tests/python/test_spec175_qwen_generation.py \
  tests/python/test_spec175_qwen_stateful_onnx.py \
  tests/python/test_spec170_plan_sealer.py
```

Result: build/import succeeded and `55 passed`. `ldd` has no unresolved entry;
the imported extension resolves `libndn-service-framework.so.0.1.0` from the
current repository build, Boost 1.71, ndn-cxx 0.9, and NDN-SVS Experimental.

## ABI boundary

The package directory also contains an older CPython-3.10-named extension. It
was not imported by host Python 3.8 and is not evidence for the future SIF.
Candidate sealing MUST resolve the exact expected SOABI/import path and MUST NOT
select the first filename matching `_ndnsf*.so`. The Python-3.10 extension used
by a SIF must be rebuilt inside that candidate SIF or an ABI-identical sealed
builder.

## Remaining closure

These checks prove the current native surface, host import, and dependency
closure. They do not yet prove a real native Provider writer delivering events
to a real native User callback/async iterator, callback thread isolation, or
writer invalidation after handler return. T009 therefore remains unchecked.
