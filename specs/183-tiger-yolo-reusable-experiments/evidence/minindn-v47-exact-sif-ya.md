# v47 exact-SIF Y-A evidence

Run `minindn-local-20260909-v47-ya44` used the unchanged base SIF
`base-runtime-controller-version-j4-v22.sif` (`sha256:2c07a9f14d48fabd9fb58036c1634f3cc3282dd28c6470add9f8a7da0cb829b5`)
and rebuilt external application bundle `app-controller-version-j4-v31`.
The dispatch plane was checked as `VERIFIED` before preparation, and the
containerized issuer returned a `PREPARED` receipt.

The real MiniNDN Y-A process returned `returncode=0`. Its retained evidence is
under:

```text
Experiments/TigerCluster/results/yolo-minindn-20260909-v47-ya44/
  minindn-local-20260909-v47-ya44/host-minindn/output/
```

The terminal subcase receipt reports `status=PASS`,
`reason=TERMINAL_RESPONSE_VERIFIED`. The numerical receipt reports:

```text
shape=[1,50,6]
matched=true
maxAbsError=0.0005340576171875
rtol=0.0001
atol=0.001
```

The User log contains `YOLO_ACK_DRIVEN_RESULT status=true`. The Provider log
contains `runnerKind=onnxruntime-cpu`, `realCompute=true`,
`loadCompleted=true`, and `warmupCompleted=true`. Earlier v45 evidence showed
that the same protocol reached real ONNX Runtime execution but failed when the
atomic FullModel published raw `[1,300,6]` predictions; v31 includes the
explicit `ONNX_POSTPROCESS` terminal contract and the candidate-construction
fix.

This evidence proves the exact-SIF local CPU APP/NDNSF-DI path for Y-A. It is
not a formal qualification: the retained run and preparation both remain
`qualification=NOT_EVALUATED`, and host qualification manifest, GPU, Tiger
allocation, and two-node gates remain open.
