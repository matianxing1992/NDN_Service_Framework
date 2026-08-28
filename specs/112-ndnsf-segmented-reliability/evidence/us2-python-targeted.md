# US2 Current Python Targeted Binding

## Decision Gate

Disposition: **no product source change required**.

The email described an older fork where Python forced Provider tokens on but
could not configure matching behavior and registered only a Normal handler. The
current compiled binding already does the required secure behavior:

- `NativeServiceProvider` calls `setUseTokens(true)`;
- `NativeServiceUser` calls `setUseTokens(true)`;
- `NativeServiceProvider::addService` registers
  `ServiceInvocationMode::NormalAndTargeted`;
- neither native Python class exposes `set_use_tokens`, so production Python
  cannot disable token enforcement.

Accordingly T023's conditional branch forbids a speculative edit to
`pythonWrapper/src/ndnsf/_ndnsf.cpp` or `pythonWrapper/ndnsf/service.py`.

Evidence level: **executed-pass (C++ security negatives + real compiled Python
binding + 0% MiniNDN)**.

## Focused Tests

```text
./build/unit-tests --run_test=GenericDynamicApi/TargetedInvocation
Running 15 test cases...
*** No errors detected
```

The focused suite includes Normal+Targeted registration with one handler,
bootstrap and fast path, missing user/provider tokens, mismatched token
bindings, consumed-token replay, wrong Provider, runtime replay, and exactly-once
handler invocation. The Spec 112 aggregate negative test invokes the handler
zero times for missing/unknown/mismatched credentials, once for a valid token
pair, and still once after replaying the consumed Provider token.

The lightweight Python test loads the real `_ndnsf` `.so`, asserts the Targeted
methods exist, asserts no public token-disable method exists, and checks Python
response conversion. It passed 3/3 before the opt-in network test.

## Real Binding MiniNDN Evidence

- Candidate: `spec112-796140a912f13c323020`
- Candidate manifest:
  `results/spec112-segmented/spec112-796140a912f13c323020/candidate-manifest.json`
  (`4701aaf2f052a73a0c317630e62b121f5a5527eabc4d47076a0eb9ec4ae16336`)
- C++ test binary:
  `d3c37ee64f37f735efac248a4ec7e5f68688284ccf77b2f6b3e1c6e20ac76343`
- Compiled Python extension:
  `cdf95ea389632bd71f62b3a05a9ad510226a2c2e20b66a0d8e08ef6ad24df22b`

Command shape:

```text
sudo -n env \
  PYTHONPATH=$PWD/pythonWrapper \
  SPEC112_RUN_TARGETED_BINDING=1 \
  SPEC112_CANDIDATE_MANIFEST=$PWD/results/spec112-segmented/spec112-796140a912f13c323020/candidate-manifest.json \
  python3 tests/python/test_ndnsf_targeted_python_api.py -v
```

| Cell | Result | Request | Handler log count |
|---|---:|---:|---:|
| `python-binding-normal` | SUCCESS | 1/1 byte-exact | 1 |
| `python-binding-targeted` | SUCCESS | 1/1 byte-exact | 1 |

The two observed request latencies were 171.730–181.694 ms. This timing is only
a focused local MiniNDN observation. Campaign summary:
`results/spec112-segmented/spec112-796140a912f13c323020/campaign-summary.json`
(`2d2797c058dd5341f67670fa71df61626dff4f820f9621b37975c86ff35e37f5`).

## Scope

No tokens-off API, compatibility alias, wire mode, or old-fork patch was added.
The existing current security model remains authoritative.
