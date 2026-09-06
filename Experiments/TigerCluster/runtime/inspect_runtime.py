"""Record the actually loaded in-SIF native dependency closure without a Face."""
import hashlib
import json
from pathlib import Path
import platform
import subprocess
import sys
import sysconfig

import ndnsf._ndnsf as native
from runtime.baseline import digest, write_json


def inspect():
    # Import both external applications before creating any forwarder. This
    # checks the real old-image API surface, not the development host packages.
    import apps.ndn_probe
    import apps.service_probe
    extension = Path(native.__file__).resolve()
    if not str(extension).startswith("/opt/venv/"):
        raise RuntimeError("EXTENSION_OUTSIDE_SIF_PREFIX")
    linked = subprocess.run(["ldd", str(extension)], text=True, capture_output=True, check=True)
    if "not found" in linked.stdout:
        raise RuntimeError("NATIVE_DEPENDENCY_MISSING")
    paths = [extension,
             Path("/opt/ndnsf-di/current/lib/libndn-cxx.so.0.9.0"),
             Path("/opt/ndnsf-di/current/lib/libndn-service-framework.so.0.1.0")]
    record = {"python": sys.version, "pythonExecutable": sys.executable,
              "soabi": sysconfig.get_config_var("SOABI"), "glibc": list(platform.libc_ver()),
              "native": {str(p): digest(p) for p in paths}, "ldd": linked.stdout}
    write_json(Path("/output/runtime.json"), record)


if __name__ == "__main__":
    inspect()
