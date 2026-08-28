# T025 Tiger control subgate — 2026-08-28

## Result

The bounded current-SIF deployment control passed on Tiger job `206103`.
This closes only the first T025 control subgate; G5 stateful Qwen3.6-27B
qualification, G6 multi-Provider execution, G6C conversation residency, and
G7 performance remain open.

| Field | Value |
|---|---|
| Candidate | `spec175-final-candidate-replay42c` |
| SIF | `sha256:63539a1adffa4d8500c56d34104d81971aa72a29958723cd35143bd52b98fbd1` |
| Gate | `control` (one node, `gres/gpu:0`, 20-minute bound) |
| Tiger job | `206103`, node `itiger03`, terminal `COMPLETED`, exit `0` |
| Terminal record | `spec175-gate-terminal.json`, SHA-256 `45c0bc8c6b9d6a563d189d23e386299a93e0721c620a386d42182ba6d89312bf` |
| Orchestration record | `orchestration-terminal.json`, SHA-256 `91d043cf2b2ba118b3b35c5f9ebb073216ea7c8277f6f6fbff192ff97251a408` |

The exact SIF was hash-verified on the remote staging path before submission.
The repository-owned checklist validator passed with no errors, and the job
used the frozen `submit.sh control` interface. The original failed job `206101`
is retained separately; it failed before SIF execution because Slurm's spool
copy could not resolve the runner by `dirname "$0"`. The submission wrapper was
fixed and regression-tested before this replacement.

## Lifecycle evidence

The user log records permission installation, four successful ACKs, and the
final response. The same request ID is visible across provider request and
selection publications.

```text
NDNSF_DI_CONTROLLER_READY
NDNSF_DI_NATIVE_PROVIDER_READY provider=/example/hello/provider/A
NDNSF_DI_NATIVE_PROVIDER_READY provider=/example/hello/provider/B
NDNSF_DI_NATIVE_PROVIDER_READY provider=/example/hello/provider/C
NDNSF_DI_NATIVE_PROVIDER_READY provider=/example/hello/provider/D
NDNSF_DI_USER_PERMISSIONS ...
NDNSF_DI_ACK_CLOSED count=4 successful=4 providers=A,B,C,D
NDNSF_DI_PROVIDER_RESPONSE provider=A
NDNSF_DI_CONTROL_LIFECYCLE_PASS acks=4 response=HELLO_FROM_A
```

All four providers published and validated the request; the selected provider
published the response. The copied raw logs and terminal records are retained
under this directory. Their SHA-256 values are:

```text
controller.log          54415402114d4098386bb71a4f562cfe5b56d229210f81d2d5ec695b020c27b3
user.log                e335550db8ab9a3fdddfda56df2d847d6db43cc510ef3c39bfeb9c55529b16a2
provider-A.log          d4a2d610c1c5b6ebb659a730b1633e1f507f57c75c5757e297d9cf2c0fffc7a7
provider-B.log          03f2186592b974a97524dbfcceffc0f2a47cb3633a4b3cf0803b52c646063f4f
provider-C.log          e32b93c89e3f34ac730b634af7c51205882d438db1e56a31c9b429476980971d
provider-D.log          3c757be2d89ed602725dce7d880423c3c6014255b0e8455399700f5c68b59618
nfd.log                 019220a02fe058a8f10c10c03b16828c64c3d5e4db218af32ba7940d4e8011b7
nfd-status.txt          53834aa556a89d3c2a449040a39cd0767fbbd1f0d0ad6dbbeccf50297bbfb2e9
control-user-result.txt 0514d9c5b1907d6b43d07a893f8c68920e7108807bd9baa741402b15eda5db95
```

The control workload uses one fresh shared KeyChain/`HOME` for the local
controller, User, and four Provider processes, with distinct identities and
controller bootstrap entries. This is required by the current permission
response implementation: the controller encrypts each permission response to
the local identity certificate. Provider starts are staggered by 0.5 seconds,
and `NDNSF_SVS_PERIODIC_SYNC_MS=100` is set in the control bundle so local
startup does not wait for the production 30-second periodic sync. These are
explicit control-workload settings, not production defaults.

The only non-fatal runtime warnings were an unavailable host multicast face and
the expected absent `/etc/ndn/ndnsf.conf`; neither prevented the authenticated
Request/ACK/Selection/Response lifecycle. No model artifact, CUDA execution,
or performance claim is included in this control result.
