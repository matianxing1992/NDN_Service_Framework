# Spec 116 Candidate Identity

## Source Candidate

- Base commit: `751384c50cb424bfaa39aefefee207b37afb4306`
- Dirty-tree source manifest SHA-256:
  `9b50a0e0a24f69f94da7fea6bccefe0932ca1b24ed3b0711eb950f808a3799b1`
- Captured: 2026-07-17
- Repository: `/home/tianxing/NDN/ndn-service-framework`

The manifest, rather than the base commit alone, identifies the tested
candidate because Spec 116 is intentionally uncommitted in a worktree that also
contains preserved user changes. It covers the DI implementation, NDNSF Core
sources, Python bindings, Python/C++ tests, both MiniNDN fixtures, and `wscript`:

```bash
{
  find NDNSF-DistributedInference ndn-service-framework \
    pythonWrapper/ndnsf pythonWrapper/src tests/python tests/unit-tests \
    -type f ! -path '*/__pycache__/*' ! -name '*.pyc' -print
  printf '%s\n' \
    Experiments/NDNSF_DI_Catalog_Minindn.py \
    Experiments/NDNSF_DI_NativeTracer_Minindn.py \
    wscript
} | LC_ALL=C sort -u | xargs sha256sum | sha256sum
```

## Built Artifacts

| Artifact | SHA-256 |
|---|---|
| `build/unit-tests` | `aa700bdd37508aca99bc3a95dad5cae550be780bbf9fb28f5f46286ee5285556` |
| `build/examples/App_ServiceController` | `118a67e783dc9770247e68e313b81082f1a57ba165077d2fb1c3fb5647c56e7d` |
| `build/examples/App_Provider` | `ab8f0a201169475d8689ebfd0ad3148b22f7b67be1ba4a55e0e8d0c65b876654` |
| `build/examples/App_User` | `909f78441702ef7bfe911e67f4b9636f8fd342bcae1333c8219d297f6e09f088` |

The MiniNDN readiness summary records the base commit because the experiment
runner cannot represent an uncommitted tree. This file supplies the controlling
source-manifest identity for that result.
