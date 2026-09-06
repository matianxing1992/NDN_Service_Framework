# NDNSF-DI current Spec/Tiger audit (2026-08-16)

## Which Spec is current?

The repository's active Spec Kit pointer is Spec 173, `Submission-Ready NDNSF
Evidence`.  Spec 173 is a paper/evidence package; its tasks T001-T011 are
complete and its final verification is a manuscript and regression audit.  It
does not close the NDNSF-DI TigerCluster feature gates.

The latest NDNSF-DI implementation/qualification specification is Spec 170,
`Reusable Canonical Model-Layer Artifacts`.  Its task ledger still has all 38
tasks open.  The Tiger gates required by that ledger are T030-T034, and they
depend on the native 3A/3B/3C implementation and local Gates A-C first.

## Candidate and evidence

The previously promoted direct-local SIF release
`spec170-runtime-e7ed-ort1200-svs1d432-20260816` was superseded and its large
SIF was removed during the 2026-08-16 storage cleanup. Its manifest and
qualification evidence remain historical records; they are not evidence for
the current source.

The current candidate is the sealed source for r5:

```text
source commit: 0142d4ee2310de056aa391d954f2c3db8fb62023
source seal: sha256:0b2ea4a56713d4a2b4e36bdc47edba568cfcb7c4dab7e1db3b2bf198d5ac5462
intended SIF: /project/tma1/ndnsf-di/releases/spec170-runtime-0142d4-gpu-20260816-r5/runtime.sif
SIF SHA-256: 490ff5fbf20ef3be56caf398478f457efc2a786fdeaf41e90d1ccbbc9addafb6
SIF bytes: 4397842432
NDN-SVS: 1d432a5b1ffde64544963a86fda9163ccc26610e
ONNX Runtime: 1.26.0
```

Tiger job 196525 produced this SIF and its static probe passed, but the build
wrapper returned `ROOTLESS_BUILD_SCRATCH_CLEANUP_FAILED` after payload
production.  The release is consequently `artifact-ready /
orchestration-failed`, not a clean wrapper PASS.  Independent exact-SIF job
196669 verified the same path and SHA without rebuilding it and passed the
runtime/Python/ORT/Torch static checks (`SPEC170_SIF_VERIFY_PASS`).

The authoritative current identity is the source-seal record in the protected
candidate/staging evidence and the exact Tiger release `SHA256SUMS`; the former
`.codex-tmp/spec170-candidate-worktree` duplicate was removed during storage
cleanup and is not cited as current evidence.

## Verified versus blocked

| Scope | Current evidence | Verdict |
|---|---|---|
| Python contract suite | 58 passed, 2 opt-in suites skipped | pass for covered contracts |
| Real local MiniNDN gate | 3 passed, 1 explicitly skipped (Qwen multi) | pass for the executed gate |
| Local NDNSF_DATA_V1/SVSPubSub L2 fixture | 7 coordinator unit cases + 1 three-face SVS integration case passed | pass for the exercised in-process transport path; not a production/Tiger feature pass |
| Tiger D0 | Job 195020 (old SIF) | historical only; current-SIF network smoke pending |
| Tiger D1 | Job 195034, H100 CUDA ORT (old SIF) | historical only; rerun pending current SIF |
| Tiger D2a | Job 195045, two H100 local visibility/runtime (old SIF) | historical only; rerun pending current SIF |
| Tiger D2b | Job 195159 proves two-provider SIF/GPU launch only | blocked as full feature |
| Tiger D2h | entrypoint/mapping prepared, no real workload | blocked |

After the storage cleanup, the current checkout's Python contract suite was
rerun against the rebuilt native extension:

```text
python3 -m pytest -q tests/python/test_spec170_*.py
58 passed, 2 skipped, 1 warning in 6.09s
```

The rebuilt Python 3.8 extension imports successfully and exposes
`make_predictive_data_name`; its `ldd` closure resolves the Experimental
NDN-SVS prefix, Boost 1.71, and the current native library without `not found`
entries.  The skips remain the explicit opt-in real-Qwen multi-request and
real-MiniNDN markers; the storage cleanup did not introduce a contract
regression.

The current source also passes the fail-closed GPU-build preflight recorded in
`preflight-gpu-build-20260816.json`.  The local Spec170 native target closure
build passes for the seven DI targets, and the full local suite passes with
479 unit cases and 12 integration cases under Experimental NDN-SVS, Boost
1.71, and ONNX Runtime 1.26.0.  The all-examples build remains blocked only by
the unrelated host `UavDroneApp` GTK/GLib linker failure; this is not treated
as a DI target pass.

The following source-seal result belongs to the earlier r1 diagnostic identity
and is retained only as historical evidence, not as the current candidate.
After rebuilding the bounded rootless source seal from candidate commit
`2852375df234c58a0624b51d1c6b85b2f3f13c4b`, the sealed archive check was
re-run with the workspace archive in the location required by the preflight
contract.  It passed with `sourceCount=8`, `archiveCount=8`,
`nativeTargetCount=32`, `repoTargetCount=4`, and `pythonPackageCount=41`.
The source-seal digest is
`sha256:3003e8ac3b3c448e09698725c983c7488ef4571f53f8e47d81c6d5a2c094e664`.
The small receipt is retained at
`results/spec170-source-seal-20260816/`; the 74 MiB workspace archive and
dependency archives remain only in protected staging and are not duplicated
under `results/`.

## Blocking implementation gap

The C++ tree contains `COLLAB-LARGE` dependency publication/fetch and local
`NativeProviderRuntime`.  The native `NDNSF_DATA_V1` framing, AEAD/HMAC,
capability/key wrapping, provider-group coordination, bounded replay window,
and an in-process SVSPubSub mapping/retry fixture now have local evidence (see
`native-data-v1-primitive-20260816.md`).  This fixture is deliberately
narrower than the qualification gate: its bridge still uses the existing large
named Data path around independently fetchable segment bundles, its manifest
signing is callback-based rather than production KeyChain signing, and its
manifest digests are not yet ciphertext digests.  It therefore cannot promote
the frozen launch-only SIF to D2b or close D2h.

## Next executable sequence

1. Replace the bridge with the production per-segment `NDNSF_DATA_V1` SVS
   publication/fetch path and add real NDN KeyChain manifest signing plus
   ciphertext-digest binding.
2. Add a real two-Provider native workload and a 3C hybrid workload, then close
   local Gates A-C without changing the candidate after freeze.
3. Build a new source/SIF identity, rerun D2b on healthy nodes, and only then
   run the fixed D2h mappings.  Keep the current D2b/D2h BLOCK evidence; do not
   reuse the old launch-only jobs as feature results.
