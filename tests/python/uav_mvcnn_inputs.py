"""Bind local MVCNN regression runs to the explicitly prepared artifact."""
import hashlib
import json
from functools import lru_cache
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory


@lru_cache(maxsize=1)
def _prepare_local_subject():
    """Keep exported inputs outside the tracked remote qualification manifest."""
    root = Path(__file__).resolve().parents[2]
    owner = TemporaryDirectory(prefix="ndnsf-test-mvcnn-")
    subprocess.run(
        [sys.executable, str(root / "NDNSF-UAV-APP/tools/prepare_mvcnn_artifact.py"),
         "--output-dir", owner.name], check=True, capture_output=True, text=True)
    manifest = json.loads(
        (Path(owner.name) / "mvcnn_vehicle_cpu.manifest.json").read_text())
    return owner, manifest


def prepared_mvcnn_arguments(directory):
    root = Path(__file__).resolve().parents[2]
    _owner, manifest = _prepare_local_subject()
    artifact = manifest["artifact"]
    path = Path(artifact["path"])
    digest = "sha256:" + hashlib.sha256(path.read_bytes()).hexdigest()
    assert digest == artifact["digest"], "prepared MVCNN artifact drift"
    assert manifest["onnxChecker"] == "passed"
    # A local export is a new subject. Do not rewrite the remote frozen registry
    # or bypass its identity check; explicitly register this test's subject.
    registry = json.loads((root / "NDNSF-UAV-APP/configs/uav_multiview_models.json").read_text())
    profile = next(p for p in registry["profiles"] if p["profile_id"] == "vehicle-mvcnn-v1")
    assert profile["source_revision"] == manifest["sourceRevision"]
    profile.update(model_digest=digest, model_artifact=str(path),
                   checkpoint_digest=manifest["checkpoint"]["digest"],
                   checkpoint_artifact=manifest["checkpoint"]["path"])
    registry_path = Path(directory) / "prepared-model-registry.json"
    registry_path.write_text(json.dumps(registry))
    return ["--model-registry", str(registry_path),
            "--model", str(path), "--model-digest", digest]
