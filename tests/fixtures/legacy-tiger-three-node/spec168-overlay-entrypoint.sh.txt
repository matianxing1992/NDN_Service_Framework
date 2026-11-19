#!/bin/bash
set -Eeuo pipefail

: "${SPEC168_OVERLAY_ROOT:?}"
test "$#" -gt 0
test ! -e "$SPEC168_OVERLAY_ROOT"
mkdir -p "$SPEC168_OVERLAY_ROOT"
for package in ndnsf py_repoclient ndnsf_distributed_inference; do
  cp -a "/opt/ndnsf-app/python/${package}" \
    "$SPEC168_OVERLAY_ROOT/${package}"
done
cp -a /source/ndnsf/ndnsf/. "$SPEC168_OVERLAY_ROOT/ndnsf/"
cp -a /source/repo/py_repoclient/. "$SPEC168_OVERLAY_ROOT/py_repoclient/"
cp -a /source/di/ndnsf_distributed_inference/. \
  "$SPEC168_OVERLAY_ROOT/ndnsf_distributed_inference/"
native_core=/source/native/lib/libndn-service-framework.so.0.1.0
shopt -s nullglob
native_python_extensions=(/source/native/python/_ndnsf.cpython-310-*.so)
shopt -u nullglob
if test -f "$native_core" || test "${#native_python_extensions[@]}" -gt 0; then
  test -f "$native_core" || {
    echo SPEC168_NATIVE_ABI_CLOSURE_MISSING_CORE >&2
    exit 1
  }
  test "${#native_python_extensions[@]}" -eq 1 || {
    echo SPEC168_NATIVE_ABI_CLOSURE_EXTENSION_COUNT=${#native_python_extensions[@]} >&2
    exit 1
  }
  native_python_extension=${native_python_extensions[0]}
  cp -a "$native_python_extension" \
    "$SPEC168_OVERLAY_ROOT/ndnsf/$(basename "$native_python_extension")"
  export SPEC168_NATIVE_CORE_LIBRARY=/source/native/lib/libndn-service-framework.so.0.1.0
  export SPEC168_NATIVE_PYTHON_EXTENSION="$SPEC168_OVERLAY_ROOT/ndnsf/$(basename "$native_python_extension")"
  export LD_LIBRARY_PATH="/source/native/lib:${LD_LIBRARY_PATH:-}"
fi
if test "${SPEC168_REQUIRE_SELECTION_FANOUT_ABI:-0}" = 1 && \
    test -z "${SPEC168_NATIVE_CORE_LIBRARY:-}"; then
  echo SPEC168_SELECTION_FANOUT_ABI_MISSING_NATIVE_CLOSURE >&2
  exit 1
fi
export PYTHONPATH="${SPEC168_OVERLAY_ROOT}:/source/llm_pipeline:/opt/ndnsf-app/python${PYTHONPATH:+:${PYTHONPATH}}"

/opt/venv/bin/python - <<'PY'
from pathlib import Path
import ndnsf
import ndnsf._ndnsf
import ndnsf_distributed_inference
import py_repoclient
import py_repoclient._py_repoclient
import hashlib
from py_repoclient import AdaptiveArtifactTransfer

root = Path(__import__("os").environ["SPEC168_OVERLAY_ROOT"]).resolve()
for module in (ndnsf, ndnsf_distributed_inference, py_repoclient):
    if root not in Path(module.__file__).resolve().parents:
        raise SystemExit(f"SPEC168_PACKAGE_OVERLAY_MISMATCH:{module.__name__}")
for module in (ndnsf._ndnsf, py_repoclient._py_repoclient):
    if root not in Path(module.__file__).resolve().parents:
        raise SystemExit(f"SPEC168_NATIVE_EXTENSION_NOT_PRESERVED:{module.__name__}")
if AdaptiveArtifactTransfer is not py_repoclient._py_repoclient.AdaptiveArtifactTransfer:
    raise SystemExit("SPEC168_REPO_ABI_SYMBOL_MISMATCH:AdaptiveArtifactTransfer")
print(
    "SPEC168_REPO_ABI_CLOSURE_PASS",
    f"extension={Path(py_repoclient._py_repoclient.__file__).resolve()}",
    "symbol=AdaptiveArtifactTransfer",
    flush=True,
)
native_core = __import__("os").environ.get("SPEC168_NATIVE_CORE_LIBRARY")
if native_core:
    expected = Path(native_core).resolve()
    expected_extension = Path(
        __import__("os").environ["SPEC168_NATIVE_PYTHON_EXTENSION"]
    ).resolve()
    if Path(ndnsf._ndnsf.__file__).resolve() != expected_extension:
        raise SystemExit("SPEC168_NATIVE_PYTHON_EXTENSION_MISMATCH")
    mappings = Path("/proc/self/maps").read_text(encoding="utf-8")
    if str(expected) not in mappings:
        raise SystemExit("SPEC168_NATIVE_CORE_OVERLAY_NOT_LOADED")
    core_digest = "sha256:" + hashlib.sha256(expected.read_bytes()).hexdigest()
    extension_digest = (
        "sha256:" + hashlib.sha256(expected_extension.read_bytes()).hexdigest()
    )
    print(
        "SPEC168_NATIVE_ABI_CLOSURE_PASS",
        f"core={expected}",
        f"extension={expected_extension}",
        f"coreSha256={core_digest}",
        f"extensionSha256={extension_digest}",
        flush=True,
    )
    if __import__("os").environ.get("SPEC168_REQUIRE_SELECTION_FANOUT_ABI") == "1":
        binary = expected.read_bytes()
        required_markers = (
            b"NDNSF_SELECTION_PROVIDER_PROJECTION",
            b"SELECTION_TARGETED_PREFETCH_ISSUED",
            b"selection published; awaiting provider status",
        )
        missing = [
            marker.decode("ascii")
            for marker in required_markers
            if marker not in binary
        ]
        if missing:
            raise SystemExit(
                "SPEC168_SELECTION_FANOUT_ABI_MARKERS_MISSING:" + ",".join(missing)
            )
        print(
            "SPEC168_SELECTION_FANOUT_ABI_PASS",
            f"core={expected}",
            f"coreSha256={core_digest}",
            f"extensionSha256={extension_digest}",
            "mode=provider-projection-targeted-prefetch-status",
            flush=True,
        )
print("SPEC168_FULL_OVERLAY_IMPORT_PASS", flush=True)
PY

set +e
"$@"
command_rc=$?
set -e
# The installed packages and immutable source files are intentionally
# read-only.  Restore owner write permission inside the container namespace,
# where the copy-up was created, before returning control to the host job.
chmod -R u+w "$SPEC168_OVERLAY_ROOT"
find "$SPEC168_OVERLAY_ROOT" -mindepth 1 -delete
rmdir "$SPEC168_OVERLAY_ROOT"
exit "$command_rc"
