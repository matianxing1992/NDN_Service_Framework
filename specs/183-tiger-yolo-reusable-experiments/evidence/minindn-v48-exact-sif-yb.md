# Exact-SIF MiniNDN Y-B: v48 / v31 application

**Run:** `minindn-local-20260909-v48-yb45`\
**Date:** 2026-09-09\
**Scope:** local exact-SIF CPU MiniNDN normal graph

The run used the immutable base SIF
`base-runtime-controller-version-j4-v22.sif` with SHA-256
`sha256:2c07a9f14d48fabd9fb58036c1634f3cc3282dd28c6470add9f8a7da0cb829b5` and
the read-only external application manifest
`sha256:39ca5bd3e8d33598145229ce9dd9bcbbb58903cb20eb110fd16443c1fd13a513`.
The application source revision was `c046778adc618d108dbaff23e82551fb85ea9447`.

The maintained `spec183_minindn.py --case Y-B` owner returned `T010_DONE` with
return code 0.  Its supervisor receipt records `processCleanup=CLEAN`, an empty
error list, and `qualification=NOT_EVALUATED`.  The lifecycle contains the ten
ordered milestones from `INPUT_REFERENCE_PUBLISHED` through
`TERMINAL_RESPONSE`; the terminal response is true with one request.  All four
Provider logs contain request-bound execution evidence: BackboneNeck and both
DetectShard roles completed ORT CPU execution, while Merge completed the native
postprocess runner.  The numerical receipt reports
`schemaVersion=spec180-yolo-numerical-v1`, `shape=[1,50,6]`,
`matched=true`, and `maxAbsError=0.0005340576171875`.

This is real cross-process MiniNDN and exact-SIF APP evidence for the normal
CPU path.  It is not a GPU, TigerCluster, or host qualification PASS.  The
source-bound host receipt still requires the same run's permission and
post-Selection dependency-failure records.
