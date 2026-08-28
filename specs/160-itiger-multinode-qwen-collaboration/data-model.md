# Data Model

## FrozenCandidate

- SIF path and SHA-256
- model path, revision, and weight SHA-256
- source bundle SHA-256
- stage artifact paths, layer ranges, byte sizes, and SHA-256 values
- policy and execution-plan SHA-256

## NodeRoleBinding

- Slurm job/allocation ID
- hostname and node address
- provider identity
- role and layer range
- CUDA device index, GPU model, and GPU UUID

One acceptance allocation has exactly three bindings with unique hostnames,
providers, roles, and GPU UUIDs.

## DependencyTransfer

- request ID and session ID
- key scope
- producer role/provider/node
- consumer role/provider/node
- planned Data name
- payload bytes, segments, and SHA-256
- publish/fetch timestamps and status

## CollaborationResult

- request/session identity
- ACK candidates and selected role/provider mapping
- three stage execution records
- two dependency transfers
- final response payload/digest
- reference top token and output shape
- terminal status and failure reason
