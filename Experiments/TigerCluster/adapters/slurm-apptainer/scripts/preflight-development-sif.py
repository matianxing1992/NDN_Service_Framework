#!/usr/bin/env python3
"""Run cheap, fail-closed checks before compiling the development SIF.

The checks inspect the rendered definition and immutable inputs. They do not
compile source or mutate a host image. When a local base SIF and Apptainer are
supplied, NumPy's wheel-private DSOs are installed only in a writable
temporary overlay and imported there, exercising the final RPATH.
"""

from __future__ import annotations

import argparse
import importlib.util
import os
import re
import shlex
import subprocess
import sys
import tarfile
from pathlib import Path
from zipfile import ZipFile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[4]
BOUNDARY = ROOT / "packaging/ndnsf-di-container/lib/spec170_sif_build_boundary.py"
NUMPY_PRIVATE_LIBS = frozenset({
    "libopenblas64_p-r0-0cf96a72.3.23.dev.so",
    "libgfortran-040039e1.so.5.0.0",
    "libquadmath-96973f99.so.0.0.0",
})


def fail(code: str, detail: object = "") -> "NoReturn":
    suffix = f":{detail}" if detail else ""
    raise SystemExit("SPEC186_PREFLIGHT_" + code + suffix)


def boundary_module():
    spec = importlib.util.spec_from_file_location("spec170_preflight_boundary", BOUNDARY)
    if spec is None or spec.loader is None:
        fail("BOUNDARY_IMPORT")
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def file_entries(definition: Path) -> list[tuple[Path, str]]:
    active = False
    entries: list[tuple[Path, str]] = []
    for raw in definition.read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if line.lower() == "%files":
            active = True
            continue
        if line.startswith("%"):
            active = False
            continue
        if not active or not line or line.startswith("#"):
            continue
        try:
            fields = shlex.split(line)
        except ValueError as error:
            fail("FILES_SYNTAX", error)
        if fields:
            entries.append((Path(fields[0]), fields[1] if len(fields) > 1 else ""))
    return entries


def _builder_post(definition: Path) -> str:
    stages = boundary_module()._parse_stages(
        definition.read_text(encoding="utf-8"))
    builders = [stage for stage in stages if stage.name == "builder"]
    if len(builders) != 1:
        fail("BUILDER_STAGE_COUNT", len(builders))
    return builders[0].sections.get("post", "")


def check_workspace_consumers(definition: Path, workspace: Path) -> list[str]:
    """Verify source paths consumed by build commands exist in workspace.tar.

    `%files` only proves that the archive itself is present.  A later `cp` or
    pip install can still name a path omitted by the source sealer, which used
    to fail after the expensive container build started.  Inspect only
    commands that consume source paths; cleanup and generated build paths are
    intentionally excluded.
    """
    consumed: list[str] = []
    for raw in _builder_post(definition).splitlines():
        line = raw.strip()
        if (not line or line.startswith("#") or line.startswith("rm ") or
                line.startswith("install -d")):
            continue
        if "cp " not in line and "pip install" not in line:
            continue
        for match in re.finditer(r"/src/ndnsf/([^\s\\'\"]+)", line):
            relative = match.group(1).rstrip(";,)")
            if (relative.endswith("/build") or relative.startswith("build/") or
                    "/build/" in relative):
                continue
            consumed.append(relative)

    try:
        with tarfile.open(workspace, "r:*") as archive:
            names = {member.name.lstrip("./").rstrip("/")
                     for member in archive.getmembers()}
    except (OSError, tarfile.TarError) as error:
        fail("WORKSPACE_ARCHIVE_READ", error)

    missing = [relative for relative in consumed
               if not any(name == relative or name.startswith(relative + "/")
                          for name in names)]
    if missing:
        fail("WORKSPACE_CONSUMER_PATH_MISSING", missing[0])
    return sorted(set(consumed))


def _base_capability_tests(definition: Path) -> list[tuple[str, str]]:
    """Extract base-owned test predicates from the rendered builder shell."""
    tests: set[tuple[str, str]] = set()
    for raw in _builder_post(definition).splitlines():
        for match in re.finditer(r"\btest\s+(-[fxd])\s+(/(?:usr|opt)/[^\s\\;]+)", raw):
            kind, path = match.groups()
            path = path.rstrip("'\")")
            if (path.startswith("/opt/ndnsf-stage") or
                    path.startswith("/opt/ndnsf-candidate") or
                    path.startswith("/opt/ndnsf-di/current")):
                continue
            tests.add((kind, path))
    return sorted(tests)


def check_base_capabilities(apptainer: Path, base_sif: Path,
                            definition: Path) -> list[tuple[str, str]]:
    """Run cheap read-only checks for every base-owned tool/path predicate."""
    tests = _base_capability_tests(definition)
    if not tests:
        fail("BASE_CAPABILITY_TESTS_EMPTY")
    script = ["set -eu"]
    for kind, path in tests:
        script.append(
            f"test {shlex.quote(kind)} {shlex.quote(path)} || "
            f"{{ echo BASE_CAPABILITY_MISSING:{path} >&2; exit 1; }}")
    command = [str(apptainer)]
    config = os.environ.get("SPEC186_APPTAINER_CONFIG", "").strip()
    if config:
        command.extend(["-c", config])
    command.extend(["exec", "--no-mount", "dev", str(base_sif),
                    "/bin/sh", "-c", "\n".join(script)])
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip().splitlines()[-1:]
        fail("BASE_CAPABILITIES", detail[0] if detail else result.returncode)
    return tests


def check_static_inputs(definition: Path) -> tuple[Path, Path, list[str]]:
    boundary_module().validate_definition(definition)
    entries = file_entries(definition)
    workspace = next((source for source, dest in entries
                      if dest == "/build-input/workspace.tar"), None)
    wheels = next((source for source, dest in entries
                   if dest == "/build-input/wheels"), None)
    if workspace is None:
        fail("WORKSPACE_ARCHIVE_UNDECLARED")
    if wheels is None:
        fail("WHEELS_UNDECLARED")
    for source, _ in entries:
        if not source.exists():
            fail("INPUT_MISSING", source)
    required = "NDNSF-DistributedInference/ndnsf-distributed-inference.pc.in"
    try:
        with tarfile.open(workspace, "r:*") as archive:
            names = {member.name.lstrip("./") for member in archive.getmembers()}
    except (OSError, tarfile.TarError) as error:
        fail("WORKSPACE_ARCHIVE_READ", error)
    if required not in names:
        fail("WORKSPACE_TARGET_INPUT_MISSING", required)

    wheel_files = sorted(wheels.glob("numpy-1.26.4-*.whl"))
    if len(wheel_files) != 1:
        fail("NUMPY_WHEEL_COUNT", len(wheel_files))
    try:
        with ZipFile(wheel_files[0]) as wheel:
            members = {Path(name).name for name in wheel.namelist()
                       if name.startswith("numpy.libs/") and not name.endswith("/")}
    except (OSError, ValueError) as error:
        fail("NUMPY_WHEEL_READ", error)
    if members != NUMPY_PRIVATE_LIBS:
        fail("NUMPY_PRIVATE_LIB_SET", sorted(members))
    text = definition.read_text(encoding="utf-8")
    expected_destination = (
        "destination = Path('/opt/venv/lib/python3.10/site-packages/numpy.libs')"
    )
    destination_count = text.count(expected_destination)
    if destination_count < 1:
        # Keep the established diagnostic code while reporting the updated
        # builder-side requirement.
        fail("NUMPY_FINAL_RPATH_RESTORE_MISSING", f"builder={destination_count}")
    # The final stage has no /build-input bind. It must receive the checked
    # private DSOs through `%files from builder`, then copy that payload into
    # the inherited NumPy site-packages directory.
    if "/opt/venv/lib/python3.10/site-packages/numpy.libs /opt/ndnsf-candidate/numpy.libs" not in text:
        fail("NUMPY_FINAL_STAGE_INPUT_MISSING")
    if "cp -a /opt/ndnsf-candidate/numpy.libs/. /opt/venv/lib/python3.10/site-packages/numpy.libs/" not in text:
        fail("NUMPY_FINAL_STAGE_COPY_MISSING")
    if "/opt/ndnsf-stage/python/numpy.libs" in text:
        fail("NUMPY_STAGING_DESTINATION")
    for target in ("ndnsf-distributed-inference", "ndnsf-distributed-inference.pc"):
        if target not in text:
            fail("WAF_TARGET_UNDECLARED", target)
    consumers = check_workspace_consumers(definition, workspace)
    return workspace, wheels, consumers


def check_base_numpy(apptainer: Path, base_sif: Path, wheels: Path) -> None:
    script = r"""
from pathlib import Path
from zipfile import ZipFile

wheel = next(Path('/build-input/wheels').glob('numpy-1.26.4-*.whl'))
destination = Path('/opt/venv/lib/python3.10/site-packages/numpy.libs')
destination.mkdir(parents=True, exist_ok=True)
with ZipFile(wheel) as archive:
    for member in archive.namelist():
        if member.startswith('numpy.libs/') and not member.endswith('/'):
            target = destination / Path(member).name
            target.write_bytes(archive.read(member))
            target.chmod(0o755)
import numpy
assert numpy.__version__ == '1.26.4', numpy.__version__
print('NUMPY_BASE_IMPORT_PASS')
"""
    command = [str(apptainer)]
    config = os.environ.get("SPEC186_APPTAINER_CONFIG", "").strip()
    if config:
        command.extend(["-c", config])
    command.extend(["exec", "--no-mount", "dev", "--writable-tmpfs",
                    "--bind", f"{wheels}:/build-input/wheels:ro", str(base_sif),
                    "/opt/venv/bin/python", "-"])
    result = subprocess.run(command, input=script, text=True, capture_output=True)
    if result.returncode != 0:
        detail = (result.stderr or result.stdout).strip().splitlines()[-1:]
        fail("NUMPY_BASE_IMPORT", detail[0] if detail else result.returncode)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--definition", required=True, type=Path)
    parser.add_argument("--apptainer", type=Path)
    parser.add_argument("--base-sif", type=Path)
    args = parser.parse_args()
    definition = args.definition.resolve()
    _, wheels, consumers = check_static_inputs(definition)
    if (args.apptainer is None) != (args.base_sif is None):
        fail("BASE_ARGUMENT_PAIR")
    if args.apptainer is not None:
        apptainer = args.apptainer.resolve()
        base_sif = args.base_sif.resolve()
        if not apptainer.is_file() or not (apptainer.stat().st_mode & 0o111):
            fail("APPTAINER_MISSING", apptainer)
        if not base_sif.is_file():
            fail("BASE_SIF_MISSING", base_sif)
        check_base_capabilities(apptainer, base_sif, definition)
        check_base_numpy(apptainer, base_sif, wheels.resolve())
    print(f"SPEC186_PREFLIGHT_PASS wheels={wheels} "
          f"workspaceConsumers={len(consumers)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
