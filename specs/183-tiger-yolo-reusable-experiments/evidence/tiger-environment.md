# Tiger environment evidence — v56

The v56 candidate was staged and executed without rebuilding the base SIF on
Tiger. Local and staged SIF bytes matched
`sha256:44b44d564c64387716a17588ea387ee2a255948ee744855291c8e7a0676907b0`
(3,586,351,104 bytes). The external APP manifest matched
`sha256:4a6c3af0c2cc24620c28519404f1a472074ebf354608372934339ae241942db7` and
was mounted read-only at `/app`. Compute Apptainer reported `1.5.3-1.el9`.

| Allocation | Nodes | Job | Environment/result |
| --- | --- | --- | --- |
| Single-node GPU | `itiger02` | `210471` | CUDA probe, NFD, capacity and signed readiness passed; model roles used CUDA and GPU UUID `GPU-0fab1e2a-dc0e-f3b0-62c0-f2dfab914341`; Merge stayed CPU |
| Two-node normal | `itiger02,itiger03` | `210472` | Distinct hosts, bidirectional signed Data/permission readiness, four roles, 9 dependency edges/request; GPUs `GPU-0fab1e2a-dc0e-f3b0-62c0-f2dfab914341`, `GPU-acfab0d6-a493-a983-5d6a-6008c44adfd4` |
| Two-node negative | `itiger02,itiger03` | `210473` | Same substrate; one exact dependency edge withheld and bounded native failure collected |
| Two-node reuse | `itiger02,itiger03` | `210474` | Independent allocation and identities; GPUs `GPU-519e5825-d84a-8e55-670c-c53433c4a71c`, `GPU-bcd15abe-d49a-9c07-d63e-acec4f7a8cbf` |

Every allocation passed the preflight for compute runtime, writable scratch
capacity, NFD, role placement, socket/route readiness and signed resource rows.
No Provider was launched on a mismatch, no host library overrode packaged
libraries, and cleanup receipts closed the private process and output trees.
The normal verdicts are the authority for permission/data readiness and CUDA;
transport inventories and component probes are retained as supporting evidence
only. Raw receipts remain under the declared project-storage run directories,
not only node scratch.
