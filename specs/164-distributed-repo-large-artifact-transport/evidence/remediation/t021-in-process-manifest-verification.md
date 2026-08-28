# T021 In-Process Manifest Verification

## Diagnosis

The first frozen campaign launched `openssl dgst -verify` once in every
signed-manifest consumer process. That process creation is not required by the
wire protocol or trust model. In the retained r1/c16 measured cell, aggregate
asymmetric verification time had a median of 279.826 ms across the 16 consumer
processes. Digest-only had zero asymmetric work.

## Change

`verify_detached_sha256_signature(payload, signature, public_key_der)` is now a
Python-visible ndn-cxx verification helper implemented in the native extension.
The benchmark still performs exactly one asymmetric root verification per
replica operation, but it no longer starts an external process. Public-key
input is the manifest publisher's PKCS#8 DER key, not the segmented Data
producer key.

The helper accepts RSA/ECDSA keys supported by ndn-cxx and fails closed for a
corrupt payload or signature. The private key remains fixture-only and is
removed after signing.

## Verification

The extension was rebuilt with the low-memory command:

```bash
cd pythonWrapper
CFLAGS='-O0 -g0' CXXFLAGS='-O0 -g0' \
  python3 setup.py build_ext --inplace --force -j1
```

Focused tests:

```text
detached signature positive/corrupt cases       1/1 PASS
file segmented producer                         1/1 PASS
performance harness                            11/11 PASS
performance analyzer                            3/3 PASS
```

Paired quick-smoke diagnostics at r1/c16 both passed:

```text
signed-manifest aggregate asymmetric verify: 8.716 ms
digest-only aggregate asymmetric verify:      0.000 ms
```

The quick smoke intentionally caps work and is not comparable as a throughput
acceptance sample. It confirms that the process-launch cost was removed; only
T024's new frozen campaign may decide SC-003. The failed pre-fix smoke caused
by initially supplying the Data producer key instead of the manifest key is
retained under
`results/spec164-remediation-signed-c16-smoke-20260730T0555Z`.
