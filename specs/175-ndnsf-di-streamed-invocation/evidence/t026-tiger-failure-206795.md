# T026 Tiger functional failure — 206795

**Date:** 2026-08-28  
**Candidate:** `spec175-final-candidate-replay42c`  
**Gate:** `multi-provider`  
**Result:** **FAIL during Controller/Provider bootstrap; no request executed**

The same exact SIF, external model root, workload, and six-invocation bundle
passed the model, functional-bundle, candidate-binding, and pre-submit checks.
Slurm job `206795` ran on `itiger02` for 34 seconds and exited with code `7`.
NFD and the Controller started, but Provider-0 failed before its readiness
marker:

```text
RuntimeError: certificate bootstrap Nack reason=150
PROVIDER_WRAPPER_CHILD_EXITED_BEFORE_READY
```

Nack reason 150 is the NDN no-route condition.  The Controller wrapper created
the Controller object and printed its READY markers before entering its event
loop, but did not call `APPController.start()`.  Its certificate-bootstrap
prefix was therefore not registered when the Provider sent its bootstrap
Interest.  The failure is a bundle lifecycle/registration ordering defect,
not a SIF, CUDA, ONNX Runtime, model, or NDNSF-DI execution result.

The complete remote logs and terminal record are retained at:

```text
/project/tma1/ndnsf-di/staging/spec175/replay42c/remote-submit-r1/outputs-r2/
```

The corrected bundle calls `controller.start()` before generating the
Controller certificate or emitting readiness.  The functional preflight now
requires `/bundle/controller-wrapper.sh`, `controller.start()`,
`controller.run()`, and a READY marker after `start()`, preventing this
ordering error from reaching another Tiger allocation.

This failure remains in the T026 audit trail and does not count as a
functional invocation or as G6 evidence.
