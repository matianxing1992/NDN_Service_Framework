# Spec170 D1 negative evidence — Controller PIB startup race (2026-08-17)

Job `197399` used the post-fix SIF
`spec170-runtime-dd5c11cc-localfix-20260817-r1` on `itiger08`. The exact SIF
stage/hash gate passed, but the network workload stopped before Provider
startup:

```text
SPEC170_D1_CURRENT_CONTROLLER_CERT_FAIL
SPEC170_D1_CURRENT_TERMINAL exit=10
App_ServiceController: PIB database cannot be initialized: database is locked
```

The failure is an orchestration race in the workload. It started
`ndnsec cert-dump` immediately after forking `App_ServiceController`, while the
Controller was still constructing its KeyChain. The Controller and cert-dump
therefore opened the same isolated Controller PIB concurrently. NFD reached
READY; no Provider request, ACK, Selection, or Response was attempted.

The workload now waits for the Controller's `ServiceController started...`
marker before opening the PIB for certificate export. This is a workload-only
candidate change; the SIF bytes and source revision are unchanged. A new
workload-bound release identity is required for the retry.

The original logs remain durable at:

```text
/project/tma1/ndnsf-di/evidence/spec170/d1-current-197399/
```

