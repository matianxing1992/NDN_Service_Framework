#!/usr/bin/env python3
"""Build both design PDFs without sharing transient LaTeX state."""
from datetime import datetime, timezone
from pathlib import Path
import shutil
import subprocess


def main():
    design = Path(__file__).resolve().parent
    root = design.parent
    run = root / ".codex-tmp" / ("design-pdf-" + datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ"))
    run.mkdir(parents=True, exist_ok=False)
    outputs = []
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
    for source, destination in outputs:
        shutil.copyfile(source, destination)
    print(run.relative_to(root))


if __name__ == "__main__":
    main()
