# Spec 170 TigerCluster D2h — heterogeneous two-Provider execution

## Candidate and runs

- Runtime release:
  `/project/tma1/spec170-r21/ndnsf-di/releases/spec170-runtime-r22-nacabe-thread-affinity-d2hproducer-20260818/`
- Runtime SIF SHA-256: `50478785fc9f7ef836f087e3f14954bba4982fb4196d3a6a20868c334f55eef0`
- Job `200993`: mapping `[1,2,1]`, `COMPLETED`, `00:04:19`, exit `0:0`
- Job `200995`: mapping `[2,1,2]`, `COMPLETED`, `00:02:31`, exit `0:0`

Evidence directories:

```text
/project/tma1/spec170-r21/ndnsf-di/evidence/spec170/d2h-hybrid-spec170-r22-d2hproducer-121-20260818-141855-121
/project/tma1/spec170-r21/ndnsf-di/evidence/spec170/d2h-hybrid-spec170-r22-d2hproducer-212-20260818-142328-212
```

Both jobs passed the exact-SIF stage barrier, ran two Provider processes on
separate Tiger nodes with CUDA visible, completed Selection and the final
Response, and passed the missing-data negative case. The `[2,1,2]` positive
trace additionally records `maxAbsoluteError=0.0` with absolute tolerance
`1e-06` and `complete=true`.

The provider logs show both local and peer dependency paths. For example, the
`[2,1,2]` run records local fetches for `s1-to-s2-d0/d1` and peer fetches for
`s0r1-to-s1` and `s2r1-to-s2r0`, followed by a complete final response.

## Workload semantics and limits

The final workload uses the current request-scoped
`CollaborationContext.allow_data`, `publish`, and `wait_one` APIs. It validates
the sealed request manifest/digest, producer identity and role-to-Provider
binding, and records local versus peer fetches. It does not independently
establish the full formal signed operation-manifest/segmented-Data contract
specified for the complete T018 implementation; that distinction remains
open in the closure audit.

Jobs `200987`–`200990` are preserved diagnostic failures from the same campaign
(stale role execution, local self-fetch, incomplete filters, and native
producer-role mapping). They were fixed before `200993`/`200995`; they are not
silently counted as successful runs.

These runs predate formal T029 freeze, so they are Tiger qualification evidence
for the current r22 release, not final frozen-candidate claims.

## Current r23 formal-path rerun (2026-08-19)

Job `201045` used the locally built r23 SIF on `itiger03,itiger11`, one GPU per
Provider, mapping `[1,2,1]`, and completed with exit `0:0`. It emitted
`SPEC170_D2H_SIF_STAGE_BARRIER_PASS`,
`SPEC170_D2H_HYBRID_WORKLOAD_PASS`, and `SPEC170_D2H_HYBRID_PASS`.

The positive trace records both local and peer `NDNSF_DATA_V1` fetches,
`SPEC170_D2H_RESPONSE complete=true`, and
`SPEC170_D2H_NUMERIC_ORACLE_PASS maxAbsoluteError=0.0
absoluteTolerance=1e-06`. The missing-data negative emitted
`SPEC170_D2H_NEGATIVE_PASS case=missing`. Evidence is stored at:

```text
/project/tma1/spec170-r21/ndnsf-di/evidence/spec170/d2h-hybrid-spec170-r23-request-id-fix-d2h-121-0311-rerun-20260818-121
```

The current r23 run used the updated multi-role/provider-binding workload
bundle; it supersedes the pre-freeze r22 qualification rows for the
formal-path check, while those rows remain historical evidence.

Job `201044` is retained as a setup diagnostic: it passed the SIF barrier but
stopped before the workload because the newly assembled r23 D2h bundle was
missing `nfd.conf`. The bundle was completed with the existing small config and
artifact files, and the subsequent `201045` rerun passed; this failure does
not indicate a runtime or protocol defect.

Job `201046` completed the missing current-r23 mapping. It used the same SIF
(`sha256:5b8bd6baaaf7288b3b593538b7c7bfa086feeca91276bef56d7b6c03e5ae9eeb`)
on `itiger01,itiger03`, one GPU per Provider, mapping `[2,1,2]`, and returned
`0:0` after `00:04:55`. Both Providers reported
`backend=onnxruntime-cuda`, `realCompute=true`, and `cpuFallbackUsed=false`.
The positive case recorded two ACKs, committed Selection, completed the final
response with `expectedScopes=5`, and passed the numeric oracle with
`maxAbsoluteError=0.0` and tolerance `1e-06`. The `missing` case emitted
`SPEC170_D2H_NEGATIVE_PASS case=missing`. Evidence is stored at:

```text
/project/tma1/spec170-r21/ndnsf-di/evidence/spec170/d2h-hybrid-spec170-r23-request-id-fix-d2h-212-08182118-20260819-212
```

Together, current r23 jobs `201045` and `201046` qualify both declared hybrid
rank mappings. They are still qualification runs rather than the separate
three-block publication-quality performance corpus required by SC-034.
