# Research: UAV Video True End-to-End Latency

## Decision 1: The current table does not contain capture-to-display latency

**Decision**: Treat the end-to-end metric as unavailable until a source frame is identifiable at acquisition and at render submission/presentation.

**Rationale**: Current code assigns `StreamChunk.captureMs = encodedOutputReadyMs`; the Ground Station decoder emits an output ordinal with `frame_correlation=ambiguous-one-to-many`. Adding 14.643 ms delivery p95, 87 ms startup, and 19 ms GUI p95 would combine different samples and meanings.

**Alternatives considered**: Rename the sum an estimate; pair decoder outputs FIFO-style. Both were rejected because they can guide optimization toward the wrong stage.

## Decision 2: Keep the existing 20 ms path as baseline, not final proof

**Decision**: The provisional current code default is POSIX encoder-pipe reads plus a bounded 20 ms partial flush. The old `stdio-batched` mode is the legacy comparison and rollback. Neither path is an accepted performance default.

**Rationale**: This change reduced first decoder output from 917 ms to 87 ms and output-gap p99 from 1066 ms to 142 ms in one matched 60-second pair, but created about 8.3 times more payload/Interest work and has not passed five-pair repeatability.

**Alternatives considered**: Declare the 20 ms setting final. Rejected because Spec 121 closed without an acceptance claim, transferred confirmatory ownership to Spec 122, and the true source-to-display metric is absent.

## Decision 3: Use an APP media-pipeline adapter

**Decision**: Introduce a narrow UAV-owned adapter around capture, encode, decode, and render capabilities; do not add codec concepts to NDNSF Stream Core.

**Rationale**: Stream Core already supplies the correct NDN mechanisms. The missing frame timestamp, PTS association, decoder lifecycle, queue policy, and GUI surface are application/media concerns.

**Alternatives considered**: Put frame IDs in Stream Core; directly rewrite both containers around one backend. The first violates ownership, while the second removes rollback and prevents one-variable evidence.

## Decision 3A: Use an independent visual identity oracle for matched baselines

**Decision**: The deterministic experiment source embeds a compact machine-readable frame marker that is recovered after decode. Runtime identity/PTS must agree with this marker.

**Rationale**: The current legacy image pipe does not return codec PTS, yet it must remain measurable as the matched baseline. An in-image oracle identifies the same frame across legacy and candidate backends without trusting either backend's queue order.

**Alternatives considered**: Compare only backend-local stage metrics; assume no encoder/decoder drops and pair by ordinal. Both fail to establish a true matched capture-to-widget metric.

## Decision 4: Probe GStreamer as the timestamp-preserving candidate

**Decision**: Run a bounded feasibility gate before integrating GStreamer. The candidate uses frame-aligned buffers and PTS through `appsink/appsrc`, with `gtkglsink` or an equivalent direct sink evaluated separately.

**Rationale**: The local machine exposes the system GStreamer 1.16 development API and the required source, encoder, parser, decoder, app, and GTK GL sink plugins. GStreamer naturally models live timestamps and bounded pipeline queues, while the current raw FFmpeg/MJPEG pipes discard association metadata.

**Alternatives considered**: In-process FFmpeg/libavcodec; retain only subprocess FFmpeg. A bounded in-process libav PTS probe is permitted only if GStreamer capability fails; it must pass the same identity/lifecycle gate before integration. The subprocess path remains rollback but cannot by itself prove production acquisition-to-output identity.

## Decision 5: Physical display time is conditional

**Decision**: Always report GUI/widget submission. Report physical presentation only when the selected sink exposes a trustworthy presentation callback/fence and its semantics are documented.

**Rationale**: `Gtk::Image::set()` and a GUI dispatcher timestamp precede compositor scan-out. Calling this physical display would overstate measurement coverage.

**Alternatives considered**: Add one assumed refresh interval; use a screenshot timestamp. Both are estimates, not per-frame presentation evidence.

## Decision 6: Optimize startup and steady playback separately

**Decision**: Evaluate decoder persistence/prewarming and key-frame readiness against startup. Evaluate access-unit packetization, queue policy, backend, and renderer against steady frame age/output gaps.

**Rationale**: The 87 ms value is a one-time first-input-to-first-output interval. The 142 ms p99 output gap is a different steady tail symptom. Mixing them would hide which mechanism helped.

**Alternatives considered**: One composite score. Rejected because a candidate could improve startup while worsening steady stalls or vice versa.

## Decision 7: Use matched evidence and resource/Interest guardrails

**Decision**: Retain a default only after five new counterbalanced matched 60-second pairs, four-pair directional agreement, separately counterbalanced trace-off/on controls, and explicit CPU/RSS/PIT/Interest/drop gates. Candidate-selection probes and Spec 121 results are exploratory and excluded from confirmatory counts.

**Rationale**: The current 20 ms result is encouraging but one pair and an 8.3-times work increase are insufficient for a default decision.

**Alternatives considered**: A large factorial matrix; select the lowest single p95. Both increase execution cost or selection bias before the dominant stage is known.

## Material Passport

- Origin Skill: experiment-agent
- Origin Mode: plan
- Origin Date: 2026-07-19
- Verification Status: UNVERIFIED
- Version Label: code_plan_v1

## Experiment Overview

- **Title**: NDNSF-UAV exact frame-age and low-latency pipeline comparison
- **Objective**: Establish exact stage attribution, identify the dominant remaining live-video delay, and retain one low-latency default without losing correctness or efficiency.
- **Hypothesis**: A timestamp-preserving access-unit pipeline plus bounded current-frame rendering will reduce steady frame age and output-gap tail more reliably than further enlarging the prefetch window.
- **Type**: simulation/benchmark

## Setup

- **Language/Framework**: C++17 UAV runtime, Python 3 MiniNDN driver, GTK/Xvfb, candidate GStreamer 1.16
- **Entry Command**: `python3 Experiments/run_spec122_video_latency_matrix.py --plan-only` until implementation supplies the frozen executor
- **Working Directory**: `/home/tianxing/NDN/ndn-service-framework`
- **Dependencies**: existing NDNSF/MiniNDN build plus the capability-checked media backend
- **Environment**: one-host MiniNDN, fixed 30 FPS deterministic source, 60-second measured window

## Inputs

| Input | Path | Description |
|---|---|---|
| Prior corrected baseline | `results/spec121-attribution-corrected-candidate2-20260718` | Legacy batching baseline |
| Current candidate | `results/spec121-bounded-provider-batching-candidate3-20260718` | Provisional 20 ms code-path evidence; exploratory only |
| Experiment driver | `Experiments/NDNSF_UAV_GUI_Minindn.py` | Existing topology and GUI launch path |

## Expected Outputs

| Output | Path | Format | Success criterion |
|---|---|---|---|
| Frozen plan | `results/spec122-*/run-summary.json` | JSON | Exact command, env, identities, and config digest present |
| Stage events | `results/spec122-*/video-stage-events.csv` | CSV | Valid identity and endpoint labels; ambiguous rows excluded with reason |
| Cell summary | `results/spec122-*/cell-summary.json` | JSON | Correctness, latency, drop, Interest, and resource gates reported |
| Paired decision | `specs/122-uav-video-end-to-end-latency/completion-summary.md` | Markdown | Five-pair decision with negative results preserved |

## Monitoring Configuration

- **Timeout**: bounded campaign timeout derived from setup plus 60-second measured window per cell
- **Monitor files**: cell log, stage CSV, lifecycle summary, process/resource samples
- **Experiment type override**: benchmark
- **Metric file**: `cell-summary.json`
- **Metric key**: `steady.captureToWidget.p95Ms`

## Analysis Plan

- **Primary metric**: paired change in valid steady capture-to-widget p95
- **Success threshold**: at least 25% improvement in four of five pairs while every SC-007 through SC-011 guardrail passes
- **Comparison**: frozen exact-identity `legacy-pipe + stdio-batched` rollback baseline versus one selected candidate under identical, counterbalanced conditions; the provisional 20 ms path is an exploratory input and exploratory cells are not reused
