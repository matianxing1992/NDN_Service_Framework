# NDNSF-UAV-APP Design Slides

This directory contains the standalone LaTeX/Beamer deck for the design and
mechanisms of `NDNSF-UAV-APP`.

Canonical files:

- `main.tex`: editable Beamer source.
- `main.pdf`: compiled 16:9 PDF.

The separate inspection update deck uses `UPDATES.tex` as its editable content.
Its concise five-slide body and editable TikZ unknown-target scene are in `inspection-illustrated.tex`; slide 4 marks gimbal hardware as an optional later upgrade, and slide 5 explains the current fixed-camera choice and proposed implementation steps.
`UPDATES_UAV.tex` includes that source under the requested export name;
`UPDATES.pdf` and `UPDATES_UAV.pdf` must contain the same slides. The 2026-09-10
revision adds simulation-to-field gaps and proposed hardware/perception validation
stages. It does not report flight qualification. See
[revision and validation record](UPDATES_UAV-review.md).

Editable PowerPoint export: [UPDATES_UAV.pptx](UPDATES_UAV.pptx). Titles, body
text, diagram labels and table text are native editable text; the scene,
protocol flow and two product photos are separate pictures. Diagram strokes
remain raster graphics, not individually editable PowerPoint drawing objects.
Rebuild from the current five-page PDF with:

```bash
python3 docs/NDNSF-UAV/slides/generate_editable_updates.py
```

The wrapper reuses the proposal's hybrid exporter without modifying it, repairs
PDF discretionary hyphens, and restores the five source hyperlinks. It prints
the isolated build/evidence directory. Re-render and review all pages after any
source/layout change; the figure crop layout is specific to this five-page deck.

The 2026-09-11 revision asks what is near an approximate map location:
camera Providers collect complementary partial views without a supplied target
class; an Analyzer associates the same object and returns a supported judgment
or uncertainty. The current five pages cover the scene, role mapping, field gaps,
optional gimbal hardware and the fixed-camera plan. The 2026-09-16 revision makes
the fixed-camera decision explicit without claiming completed calibration or
flight validation. The removed physical-loop and evaluation pages remain absent.

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
