# NDNSF Refactor Rules

## Core Architecture

- Use unified serviceName only.
- serviceName is a full endpoint path, e.g.:
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

## Refactor Rules

- Keep changes incremental.
- Do not rewrite the whole framework.
- Prefer wrapping old runtime behavior instead of replacing it.
- Preserve RequestMessage / ResponseMessage compatibility.
- Preserve old request/response naming format compatibility.
- Keep transport/security/discovery logic pluggable through callbacks.

## Current Goal

Current next step:
- thin PublishMessage integration
- do not rewrite NAC-ABE
- do not rewrite SVS
- do not rewrite IMS

Goal:
connect old runtime incrementally into standalone User.hpp / Provider.hpp.