# Spec170 r4 SIF library-closure audit — 2026-08-17

This is a negative release-gate result for the local r4 SIF. It does not
invalidate the local Python import or the CUDA/ONNX Runtime smoke; it shows
that the candidate is not yet a self-contained, promotion-safe runtime.

## Candidate

```text
release:  spec170-runtime-edd1e096-role-evidence-python-20260817-r4
source:   edd1e0965e688f996fb5f37ce80043ce67aa19ed
SIF:      /tmp/spec170-release-edd1e096-role-evidence-python-20260817-r4/runtime.sif
SIF SHA:  585edf7805c1ffd57e72053fdbecf832da1730c70b0dc1def1caa86517ed9926
provider: cd3f69615662c1fe40a61b63f0589c39c3b5810c546946064b2c20896f861d68
python:   78b4cd6a1669b25089153659e027fe5700709cf42137cc9602629e96c6afe5d3
```

## Checks

The skill checker was run against the exact Provider and Python 3.10
extension:

```text
python3 ~/.codex/skills/itiger-ndnsf-ops/scripts/validate-sif-library-closure.py \
  --sif runtime.sif \
  --provider /opt/ndnsf-di/current/bin/di-native-provider \
  --extension /opt/venv/lib/python3.10/site-packages/ndnsf/_ndnsf.cpython-310-x86_64-linux-gnu.so \
  --lock <sealed-library-lock.json> \
  --output sif-library-closure.json
status: FAIL
```

The checker found five forbidden build-host RUNPATH entries:

- `/tmp/spec170-svs-install-20260817/lib` in the Provider;
- `/tmp/spec170-fixed-source-20260817/build` in `_ndnsf.so`;
- the checkout `build` directory;
- the checkout `.codex-tmp/spec173-toolchain/ndn-svs-prefix/lib`;
- the checkout `.local-boost171/lib`.

The r4 release directory also has no sealed library-lock JSON. Under the
updated skill this is an independent hard failure; the placeholder above is
intentional and must be replaced by a real lock for the rebuilt candidate.

The SIF did contain 18 inspected packaged shared-library entries and all
observed `ldd` dependencies resolved, but those facts do not override a
host-only RUNPATH. The candidate must be rebuilt with release-only paths such
as `/opt/ndnsf-di/current/lib:/opt/onnxruntime/lib`, then receive a new SIF
identity and a fresh closure JSON before another Tiger submission.

## Disposition

`BLOCKED_LOCAL_LIBRARY_CLOSURE`. Do not suppress the RPATH row, reuse the r4
release identity, or infer promotion parity from the earlier import/`ldd`
probe. The r4 Tiger attempt remains useful runtime evidence, but it is not a
qualified release candidate.
