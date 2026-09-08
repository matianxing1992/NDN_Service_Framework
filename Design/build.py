#!/usr/bin/env python3
"""Build both design PDFs without sharing transient LaTeX state."""
from datetime import datetime, timezone
from pathlib import Path
import shutil
import subprocess
import json
from design_state import build_inputs, digest


def main():
    design = Path(__file__).resolve().parent
    root = design.parent
    run = root / ".codex-tmp" / ("design-pdf-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ"))
    run.mkdir(parents=True, exist_ok=False)
    outputs = []
    inputs = build_inputs(design)
    for name in ("current-design", "target-design"):
        out = run / name
        out.mkdir()
        for iteration in (1, 2, 3):
            with (out / f"pass-{iteration}.log").open("wb") as log:
                subprocess.run(["/usr/bin/xelatex", "-no-shell-escape", "-interaction=nonstopmode",
                                "-halt-on-error", "-file-line-error", f"-output-directory={out}",
                                f"{name}.tex"], cwd=design, stdout=log, stderr=subprocess.STDOUT,
                               check=True)
        outputs.append((out / f"{name}.pdf", design / f"{name}.pdf"))
    if inputs != build_inputs(design):
        raise RuntimeError('Document inputs changed during PDF build')
    for source, destination in outputs:
        shutil.copyfile(source, destination)
    record = dict(inputs=inputs, pdfs={p.name: digest(p) for _, p in outputs},
                  run=str(run.relative_to(root)))
    (design/'build-provenance.json').write_text(json.dumps(record, indent=2)+'\n')
    (run/'build-provenance.json').write_text(json.dumps(record, indent=2)+'\n')
    print(run.relative_to(root))


if __name__ == "__main__":
    main()
