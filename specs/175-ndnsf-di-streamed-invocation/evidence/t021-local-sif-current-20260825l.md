# T021 local SIF and two-layer preflight evidence

Date: 2026-08-25
Candidate: `spec175-final-candidate-20260825l`

## Candidate identity

- Apptainer: `/opt/apptainer/1.5.3/bin/apptainer` (1.5.3)
- SIF: `<local-scratch-root>/spec175-final-candidate-20260825l/spec175-runtime-g4final.sif`
- SIF SHA-256: `sha256:250d510ae13b1d8567b0d83935df70fa14e7bf9dc46fbecbecbee40548262cd8`
- Complete candidate source seal: `sha256:ddb90bb790a81bc5a4d3cca43b7cfc85cdb5fe1180de35ca8ee81147c22c8a9d`
- Host-G3 dirty-source seal: `sha256:af74ac98560c53695c636ed9950c7df13b886e6a989394c62a9240e12a6bb0fe`
- Host-G3 manifest: `results/spec175/g3/host-minindn-manifest-current-20260825l.json`

The two source seals are intentionally different records. The host manifest
binds the G0 dirty-source overlap used to qualify G3; the SIF label binds the
complete sealed candidate source. The strict builder and replay driver reject
using one record in place of the other.

## SIF-owned runtime preflight

Record: `results/spec175/g4/sif-runtime-preflight-current-20260825l.json`

Status: **PASS**.

The exact SIF contains the NFD, `nfdc`, `ndnsec`, Controller, repository,
Provider, User, CPython 3.10 binding, and ONNX Runtime runtime. Python is
3.10.18; ONNX Runtime is 1.20.0 with TensorRT, CUDA, and CPU providers;
`_ndnsf.so` and `di-native-provider` have complete `ldd` closure. The runtime
scan found no MiniNDN/Mininet/OVS/NLSR tools or modules and no PyTorch,
Transformers, or functorch deployment residue.

## Host-substrate preflight

Record: `results/spec175/g4/host-substrate-preflight-current-20260825l.json`

Status: **PASS**.

The host owns MiniNDN/Mininet, Open vSwitch, NLSR, `mnexec`, `ip`, topology,
namespace privilege, the replay driver, and Apptainer 1.5.3. The preflight
verified the production runner contract, three repetitions per M01--M10, and
the required 30-entry host gate. It does not substitute a host-built NDNSF
extension or host application process for the SIF runtime.

## Boundary

The host driver creates namespaces and links. Every NFD and NDNSF application
in G4 is launched through `apptainer exec --cleanenv` using this exact SIF.
MiniNDN is therefore an orchestration substrate, not a deployment payload.
The exact 30-case replay is recorded separately as T023/G4 evidence and is
not implied by these preflight PASS records.
