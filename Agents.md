# NDNSF Refactor Rules

## Core Architecture

- Use unified serviceName only.
- serviceName is a full endpoint path, for example:
  - /ObjectDetection/YOLOv8
  - /LLM/Llama3/Prefill
- Do not internally depend on separate ServiceName + FunctionName.
- Old request/response naming compatibility must still work.
- Always recombine parsed old ServiceName + FunctionName into unified serviceName before dispatch.

## Current Standalone Runtime

Standalone local runtime exists in:

- CodeGenerator/Generated/User.hpp
- CodeGenerator/Generated/Provider.hpp

Current features already implemented:

- typed protobuf asyncCall/addHandler
- request/response local dispatch
- old-compatible request/response name construction/parsing
- ACK skeleton
- FirstResponding / LoadBalancing / NoCoordination local selection skeleton
- custom AcksHandler override support
- standalone local loopback test

## Publish Boundary

PublishMessageBridge is now the unified publish boundary.

Current publish-connected paths:

- User request publication
- Provider ACK publication
- Provider response publication

All currently operate in local/mock mode.

## Runtime Integration Direction

Future NAC-ABE/SVS/IMS integration must happen through backend/runtime layers attached to PublishMessageBridge.

Do not move NAC-ABE, SVS, IMS, validator, or transport logic directly into:

- User.hpp
- Provider.hpp

Keep User.hpp and Provider.hpp focused on:

- generic RPC flow
- request/response dispatch
- ACK coordination
- provider selection
- protobuf handling

## Refactor Rules

- Keep changes incremental.
- Do not rewrite the whole framework.
- Prefer wrapping old runtime behavior instead of replacing it.
- Preserve RequestMessage / ResponseMessage compatibility.
- Preserve old request/response naming format compatibility.
- Preserve ACK naming compatibility.
- Keep transport/security/discovery logic pluggable through callbacks or backend layers.

## Current Goal

Current next step:

- Runtime backend design for PublishMessageBridge
- incremental NAC-ABE/SVS integration through backend layers only

Do not rewrite:

- NAC-ABE
- SVS
- IMS

Goal:

Connect old runtime incrementally into standalone User.hpp / Provider.hpp through reusable backend abstractions.