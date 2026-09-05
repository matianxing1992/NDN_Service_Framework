# Implementation Plan: UAV Multi-View Collaborative Recognition

**Branch**: `UAV-Experimental` | **Date**: 2026-08-28 | **Spec**: [spec.md](spec.md)

**Input**: Feature specification from `specs/177-uav-multiview-collaborative-recognition/spec.md`

## Summary

Extend the Spec 176 mission/job lifecycle with a bounded multi-view recognition job. UAVs publish immutable signed image Data; the request carries exact references and compact capture metadata. A selected GPU Provider fetches and verifies the views, runs per-view detection followed by MVCNN-style feature pooling across detected target crops, publishes one fused result manifest and one annotated image per contributing view, and returns only exact output references through NDNSF. The generated six-view car fixture is a functional gate. Quantitative claims require a registered synchronized dataset such as CoPerception-UAV.

## Technical Context

**Language/Version**: C++17 for NDNSF/UAV orchestration; Python 3.10 for the model adapter and evaluation harness

**Primary Dependencies**: current NDNSF dynamic runtime and Python bindings, ndn-cxx, NDN-SVS, OpenCV, PyTorch for the MVP multi-view model, and the existing UAV YOLO worker boundary; ONNX export is a later optimization, not an MVP dependency

**Storage**: immutable NDN Data plus local bounded model/dataset caches; no database required

**Testing**: Boost unit tests, pytest contract/integration tests, real Python model smoke, MiniNDN multi-process tests, controlled dataset evaluation

**Target Platform**: Linux CPU for deterministic contract/integration gates; CUDA GPU Provider for the registered performance model

**Project Type**: C++/Python distributed application and research evaluation

**Performance Goals**: functional fixture completes within the job deadline; stage timings and bytes are measured before setting a publication threshold

**Constraints**: at least two accepted views for a multi-view claim; exact-name signed Data; no image bytes in service payloads; one selected terminal Provider; generated images cannot support accuracy claims

**Scale/Scope**: MVP uses 2-6 views of one target in one capture window; later evaluation uses registered multi-UAV datasets and may expand view count

## Constitution Check

### Pre-design gate

- **Canonical Dynamic Runtime — PASS**: uses unified service names and current collaboration APIs; no generated stub path.
- **Security Is Part Of The Data Path — PASS**: all input and output artifacts cross existing signer, digest, authorization, token, replay, and Provider-permission checks.
- **CodeGraph First — PASS**: current `UavDetectorProvider` and Spec 176 lifecycle were inspected before this plan.
- **Spec-Driven Durable Work — PASS**: Spec 177 defines requirements, research, contracts, tasks, and audit before implementation.
- **Right-Scope Verification — PASS**: verification proceeds unit -> integration -> real model -> MiniNDN -> controlled experiment.
- **Cohesive Tasks — PASS**: tasks group tests, implementation, and focused evidence by behavioral outcome.

### Post-design gate

PASS. The design extends existing boundaries instead of introducing a second lifecycle or security path. No constitution exception is required.

## Research Decisions

See [research.md](research.md).

- **MVP algorithm**: detector-guided MVCNN-style feature aggregation over 2-6 object crops.
- **Why**: it genuinely consumes multiple views, tolerates partial and uncalibrated views, and can run on the generated fixture.
- **Research extension**: Where2comm on CoPerception-UAV for calibrated collaborative 3D detection and communication-aware comparison.
- **Non-choice**: independent YOLO detections with label voting alone are a baseline, not the claimed multi-view algorithm.

## Architecture

```text
MissionSession (long-lived, Spec 176)
  |
  +-- MultiViewRecognitionJob (bounded)
        |
        +-- ViewEvidenceReference[]
        |     UAV-A /.../IMAGE/... exact Data
        |     UAV-B /.../IMAGE/... exact Data
        |     UAV-C /.../IMAGE/... exact Data
        |
        +-- NDNSF request-scoped Provider selection
        |
        +-- selected Provider
              fetch -> verify -> correlate -> detect/crop
                    -> joint feature pooling -> fused decision
                    -> annotate each contributing view
                    -> publish result manifest + annotated Data
        |
        +-- coordinator fetches and verifies named outputs
```

### Ownership boundaries

- **UAV producer** owns original image Data and capture metadata.
- **Incident coordinator** owns job correlation, minimum-view policy, deadline, and terminal acceptance.
- **Selected Provider** owns verified view assembly, inference, annotation, and immutable output publication.
- **NDNSF** owns request-scoped Provider selection, authorization, token/replay checks, and delivery.
- **Model adapter** owns tensor preprocessing, per-view features, pooling, classification, and annotation metadata; it does not own NDN transport or terminal job state.

## Project Structure

```text
NDNSF-UAV-APP/
├── shared/
│   ├── UavProtocol.hpp/.cpp                 # multi-view job and result wire objects
│   ├── UavDetectorProvider.hpp/.cpp         # verified view-set execution boundary
│   ├── UavMultiViewRecognition.hpp/.cpp     # algorithm-independent orchestration
│   └── UavNames.hpp                         # exact input/output naming helpers
├── ground-station/
│   └── UavIncidentCoordinator.hpp/.cpp      # bounded job creation and output acceptance
├── tools/
│   ├── multiview_recognition_worker.py      # real model adapter
│   ├── run_multiview_fixture.py             # local functional runner
│   └── run_uav_multiview_minindn.py         # multi-process network scenario
└── testdata/multiview-car/                  # generated functional fixture

tests/
├── unit-tests/uav-multiview-recognition.t.cpp
├── python/test_uav_multiview_contract.py
├── python/test_uav_multiview_model.py
└── minindn/test_uav_multiview_recognition.py
```

**Structure Decision**: Reuse the current `NDNSF-UAV-APP` C++/Python split. Add one multi-view domain layer rather than placing model-specific logic in `UavDetectorProvider` or creating a second network protocol.

## Model Execution Contract

The model adapter receives a manifest plus local verified image paths. It must:

1. decode and normalize every accepted view;
2. produce per-view detections and target crops;
3. extract one feature vector per contributing crop;
4. aggregate features across views with a registered permutation-invariant pooling operator;
5. produce one fused class/identity decision and confidence;
6. return per-view boxes and overlay metadata for annotation;
7. record model digest, preprocessing profile, accepted/rejected view IDs, and stage timings.

The Provider rejects an adapter response that claims multi-view success with fewer than the registered minimum views or without evidence that the fusion stage consumed those views.

## Test Strategy

1. **Unit**: serialization, validation, deduplication, time correlation, distinct-producer policy, output completeness, and stage failure taxonomy.
2. **CPU integration**: deterministic fake adapter consumes 2-6 images and proves orchestration, security boundaries, and one terminal owner without GPU/model variability.
3. **Real algorithm**: run the MVCNN-style adapter on the six generated car views; compare 1-view and 2/4/6-view executions, verify fused provenance and six annotations.
4. **MiniNDN**: multiple UAV producer processes publish image Data; the coordinator requests recognition; the selected Provider retrieves and publishes outputs over NDN.
5. **Controlled evaluation**: register CoPerception-UAV or a deterministic AirSim/CARLA render with synchronized views, camera poses, boxes, and fixed splits; report paired single- versus multi-view metrics.

## Evaluation Registration

Before quantitative runs, freeze source/build hashes, model/checkpoint/preprocessing digests, dataset split and camera metadata, the one-view selection rule, N-view grouping rule, primary metric, uncertainty method, network configuration, and result/trace hashes.

## Complexity Tracking

No constitution violations. The real model adapter remains a separate process because it has a materially different Python/GPU dependency and failure boundary from the C++ NDNSF runtime.
