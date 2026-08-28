# Spec170 local candidate r15 build failure

- Status: `FAIL` during target-graph construction, before compilation
- Definition SHA-256: `sha256:a48fa2b54132ddc7ce7b6a19c2b8ff6577dccdba4a4eb21a2289cb3c5ff43df4`
- Build-boundary and explicit dependency/configuration closure checks: `PASS`
- Waf configure: `PASS`, including `gtkmm-3.0`, ONNX Runtime, Boost 1.71,
  protobuf, NDN-CXX, NDN-SVS, NAC-ABE, NDNSD, and OpenSSL
- Failure: `No wscript file in directory /src/ndnsf/NDNSF-DistributedRepo`
- Classification: sealed source omitted an unconditionally recursed build-graph
  file; no SIF was produced and no Tiger job was submitted.
- Correction rule: the source-seal producer must recursively discover and
  include every constant-path `bld.recurse(...)` child `wscript`, and fail
  before Apptainer if any child is missing.
