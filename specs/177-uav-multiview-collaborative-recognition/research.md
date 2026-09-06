# Research: Multi-View Algorithms for the NDNSF-UAV Use Case

**Research question**: Which existing algorithm can consume multiple images of the same target from different UAV viewpoints, produce a genuinely fused recognition result, and fit an NDNSF Provider without requiring physical cameras for the first implementation gate?

**Search date**: 2026-08-28

**Sources searched**: Computer Vision Foundation proceedings, NeurIPS proceedings, official project repositories, the official CoPerception-UAV dataset page, and official CARLA documentation.

## Decision

Use a **detector-guided MVCNN-style feature aggregation pipeline** for the Spec 177 MVP:

1. run the same detector/backbone on every view;
2. crop or mask the candidate vehicle in each accepted view;
3. extract per-view features;
4. apply a registered permutation-invariant pooling operator across views;
5. classify the pooled representation once;
6. project the fused label and confidence back onto every contributing view.

This is a real multi-view algorithm because the terminal representation and decision are computed jointly from the view set. It is compatible with the generated fixture because it does not require calibrated camera poses.

## Rationale

Su et al.'s MVCNN combines information from multiple rendered views into one compact shape descriptor and reports improved recognition as more views are provided. Its view-pooling idea directly matches the bounded 2-6-view NDNSF job. The original work targets object recognition rather than scene-level localization, so Spec 177 pairs it with per-view detection and uses the fused representation for vehicle recognition.

RotationNet is a credible alternative when viewpoint estimation is part of the research question. It accepts partial multi-view sets and jointly estimates pose and category, but it adds viewpoint hypotheses and training complexity not required for the first NDNSF demonstration.

Where2comm is the stronger research extension for collaborative UAV perception. It communicates selected spatial features and evaluates camera-only 3D detection on CoPerception-UAV. It is not the first implementation gate because it requires synchronized camera geometry, dataset-specific preprocessing, and a heavier training/checkpoint stack.

## Alternatives Considered

| Approach | Multi-view property | Calibration | Generated fixture | Decision |
|---|---|---:|---:|---|
| Independent YOLO plus majority vote | Late decision aggregation only | No | High | Baseline only |
| MVCNN-style feature pooling | Joint pooled representation | No | High | MVP |
| RotationNet | Joint category and viewpoint inference | Latent predefined viewpoints | Medium | Secondary algorithm |
| MVDet-style ground-plane fusion | Perspective features fused in a common plane | Yes | Low | Calibrated extension |
| Where2comm | Communication-aware intermediate feature fusion | Yes for UAV 3D detection | Low | Research extension |

## Dataset Decision

### Functional gate

Use `NDNSF-UAV-APP/testdata/multiview-car/`. The six generated images exercise actual decode, model, fusion, annotation, named-Data, and application paths. They do not provide physically exact geometry and cannot support accuracy claims.

### Quantitative gate

Use the official CoPerception-UAV dataset or a deterministic AirSim/CARLA renderer. CoPerception-UAV provides synchronized images from coordinated UAVs, vehicle boxes, semantic labels, and camera metadata. A controlled renderer may be used for a smaller car-specific benchmark if its scene, poses, seeds, and labels are frozen.

## Evaluation Questions

- Does the fused model outperform the same model using one registered view on the same target samples?
- How does performance change for 1, 2, 4, and 6 accepted views?
- What happens when a near view is occluded and a far view remains visible?
- What are the added NDN bytes and end-to-end latency per accepted view?
- Does the selected Provider publish exactly one terminal result and one annotation per contributing view?

## Source Verification

All included sources were verified against primary proceedings, official project documentation, or the official dataset page. They support algorithm and dataset capability claims; they do not establish that current NDNSF code has implemented those algorithms.

## Annotated Sources

1. Su, H., Maji, S., Kalogerakis, E., & Learned-Miller, E. (2015). *Multi-view convolutional neural networks for 3D shape recognition*. ICCV. https://openaccess.thecvf.com/content_iccv_2015/html/Su_Multi-View_Convolutional_Neural_ICCV_2015_paper.html
   - Establishes joint view pooling for object recognition.
   - Limitation: not by itself a UAV scene detector.

2. Kanezaki, A., Matsushita, Y., & Nishida, Y. (2018). *RotationNet: Joint object categorization and pose estimation using multiviews from unsupervised viewpoints*. CVPR. https://openaccess.thecvf.com/content_cvpr_2018/html/Kanezaki_RotationNet_Joint_Object_CVPR_2018_paper.html
   - Supports partial multi-view inference and joint category/viewpoint estimation.
   - Limitation: object-centered task and greater training complexity.

3. Hu, Y., Fang, S., Lei, Z., Zhong, Y., & Chen, S. (2022). *Where2comm: Communication-efficient collaborative perception via spatial confidence maps*. NeurIPS. https://proceedings.neurips.cc/paper_files/paper/2022/hash/1f5c5cd01b864d53cc5fa0a3472e152e-Abstract-Conference.html
   - Directly studies multi-agent collaborative perception and UAV camera-based detection.
   - Official code: https://github.com/MediaBrain-SJTU/Where2comm

4. CoPerception-UAV authors. (2022). *CoPerception-UAV: A virtual collaborative perception dataset for UAVs*. https://siheng-chen.github.io/dataset/coperception-uav/
   - Provides synchronized UAV imagery, vehicle boxes, semantic labels, and camera metadata.
   - Limitation: simulated rather than physical-flight data.

5. CARLA Simulator. (2026). *Sensors reference*. https://carla.readthedocs.io/en/latest/ref_sensors/
   - Documents camera frame, timestamp, transform, resolution, and field-of-view metadata needed for a controlled dataset.
   - Limitation: simulator metadata does not validate a recognition model.

## Search Limitations

- This was a focused design search, not a systematic review of every collaborative-perception model.
- The selected MVP optimizes implementation fit and auditability, not state-of-the-art 3D detection accuracy.
- A model/checkpoint license and reproducible training or fine-tuning recipe must be frozen before quantitative evaluation.
