# Contract: YOLO SIF+APP Pair

## Build and promotion

1. prepare-development-handoff.py validates four source repositories, sealed archives, wheels, lock and base digest.
2. build-local-sif.sh builds the candidate with Apptainer 1.5.3 from a regular localimage base.
3. build-sif-app.py validates the candidate and publishes an immutable APP tree plus manifest.
4. run-sif-app.sh validates the pair again and mounts only declared APP, base, model, artifact, identity and NFD inputs.

The existing scripts remain the only maintained implementation. A wrapper may compose them, but must not duplicate validation or silently fill paths from the host.

## Local acceptance

The maintained YOLO MiniNDN harness owns MiniNDN/NFD setup and child process lifecycle. A registered C++ selector named Spec187YoloMiniNdn owns native request, ACK, Selection, Provider execution, terminal Response, negative boundary and cleanup assertions. Python output is orchestration evidence only.

## Invalidation matrix

| Changed plane | Earliest gate |
| --- | --- |
| source, Core/DI ABI or dependency lock | source seal and candidate build |
| definition, base SIF or Apptainer | candidate build and APP materialization |
| APP files or manifest | APP validation and local run |
| model, profile, identity, NFD or mount | local run preflight |
| local run harness or selector | convergence audit and local run |
| cluster job or scheduler parameters | cluster preflight |

## Failure status

Missing or changed inputs use WAITING_EXTERNAL_INPUT or PARTIAL. Protocol failures use UNQUALIFIED until the C++ oracle and cleanup evidence are complete. No startup or scheduler marker is a protocol PASS.
