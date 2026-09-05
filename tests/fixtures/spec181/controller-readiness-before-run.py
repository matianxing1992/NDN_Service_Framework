"""Real native readiness probe, launched in the maintained Controller node.

Pass the ordinary controller.py arguments. The parent must own the isolated
network, generated keychains, child environment, and process cleanup.
"""
from pathlib import Path
import runpy
import sys
import time

from ndnsf import ServiceController

start_background = ServiceController.start_background


def checked_start(self):
    begin = time.monotonic()
    ready = self._native.wait_until_ready(80)
    elapsed = time.monotonic() - begin
    print(f"SPEC181_READINESS_BEFORE_THREAD ready={ready} elapsed={elapsed}",
          flush=True)
    if ready or elapsed < 0.06:
        raise RuntimeError("SPEC181_READINESS_PREMATURE_TERMINAL")
    return start_background(self)


ServiceController.start_background = checked_start
root = Path(__file__).resolve().parents[3]
controller = root / "examples/python/NDNSF-DistributedInference/yolo_2x2/controller.py"
sys.argv[0] = str(controller)
runpy.run_path(str(controller), run_name="__main__")
