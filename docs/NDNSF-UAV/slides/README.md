# NDNSF-UAV-APP Design Slides

This directory contains the standalone LaTeX/Beamer deck for the design and
mechanisms of `NDNSF-UAV-APP`.

Canonical files:

- `main.tex`: editable Beamer source.
- `main.pdf`: compiled 16:9 PDF.

The separate inspection update deck uses `UPDATES.tex` as its editable content.
Its concise four-slide body and editable TikZ car-inspection scene are in `inspection-illustrated.tex`; the last slide introduces candidate gimbal hardware.
`UPDATES_UAV.tex` includes that source under the requested export name;
`UPDATES.pdf` and `UPDATES_UAV.pdf` must contain the same slides. The 2026-09-10
revision adds simulation-to-field gaps and proposed hardware/perception validation
stages. It does not report flight qualification. See
[revision and validation record](UPDATES_UAV-review.md).

The latest 2026-09-11 revision removes former pages 4 and 5 (physical loop and
evaluation), as requested. It retains the task, NDNSF rationale, simulation gaps,
and product-photo page, now numbered 1 through 4.

To regenerate the update deck without mixing temporary files into the source:

```bash
# From this directory; create a unique output directory first.
pdflatex -interaction=nonstopmode -halt-on-error -output-directory=<run-dir> UPDATES_UAV.tex
pdflatex -interaction=nonstopmode -halt-on-error -output-directory=<run-dir> UPDATES_UAV.tex
# After checking the final log and rendering all pages, copy the resulting PDF
# to both UPDATES_UAV.pdf and UPDATES.pdf.
```

Build with:

```bash
cd /home/tianxing/NDN/ndn-service-framework/docs/NDNSF-UAV/slides
pdflatex -interaction=nonstopmode -halt-on-error main.tex
pdflatex -interaction=nonstopmode -halt-on-error main.tex
```

The deck is implementation-grounded. Its main sources are:

- `NDNSF-UAV-APP/README.md`
- `NDNSF-UAV-APP/shared/UavNames.hpp`
- `NDNSF-UAV-APP/shared/UavProtocol.*`
- `NDNSF-UAV-APP/drone/DroneServiceContainer.inc.hpp`
- `NDNSF-UAV-APP/ground-station/GroundStationServiceContainer.inc.hpp`
- `Experiments/NDNSF_UAV_GUI_Minindn.py`
- `ndn-service-framework/Stream.*`
- `specs/118-uav-stream-session-key/`
- `specs/119-ndnsf-stream-live-prefetch/`
- `specs/120-uav-unified-video-object/`
- `specs/121-ndnsf-stream-latency-attribution/`
- `specs/122-uav-video-end-to-end-latency/`
- `specs/123-stream-prefetch-retention-recovery/`
- `results/spec123-paper-prefetch-sample-reserve-20260719-060053/`
- `results/spec157-uav-stage-latency-diagnostic-20260726T204922Z/`
- `specs/157-uav-stage-latency-attribution/`
- `specs/069-uav-operational-layer/`
- `specs/070-uav-qgc-parity-boundary/`

This deck is independent from the PhD proposal-defense slides. Updating it must
not modify `docs/PAPER/proposal-defense/slides` unless explicitly requested.
