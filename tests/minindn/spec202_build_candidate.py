#!/usr/bin/env python3
"""Rebuild and attest the native/binding composition used by the shared runner."""

import argparse
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import time

import spec193_preflight as preflight


ROOT = Path(__file__).resolve().parents[2]
TARGETS = preflight.BUILD_TARGETS
NATIVE_OUTPUTS = preflight.BUILD_OUTPUTS
INSTALLED_LIBRARIES = ("libndn-service-framework.so", "libndnsf-distributed-inference.so")
LOCAL_NDN_CXX_HEADERS = "dependencies/ndn-cxx-8296fc9462c7ef9635b7c45468c067fd39514e31/ndn-cxx"


def hashes(paths):
    return {str(path): preflight.sha256(preflight.require_file(path, "BUILD_INPUT"))
            for path in sorted(set(paths))}


def sdk_inputs(source, build):
    config = build / "config.hpp"
    if not config.is_file():
        config = source / "ndn-svs/config.hpp"
    paths = [build / "libndn-svs.so", config, source / "wscript",
             Path("/usr/local/share/ndnsf/global-dependency-identity.json"),
             ROOT / LOCAL_NDN_CXX_HEADERS / "security/pib/impl/pib-sqlite3.hpp"]
    for directory in (source / "ndn-svs", ROOT / LOCAL_NDN_CXX_HEADERS,
                      Path("/usr/local/include/ndn-cxx"),
                      Path("/usr/local/include/nac-abe"),
                      Path("/usr/local/include/ndnsd"), Path("/usr/local/include/openabe")):
        if not directory.is_dir():
            raise RuntimeError("missing declared SDK headers: " + str(directory))
        paths.extend(path for path in directory.rglob("*")
                     if path.is_file() and (path.suffix in (".h", ".hpp") or
                         (directory == source / "ndn-svs" and path.suffix == ".cpp")))
    for tool in ("c++", "ld", "ar", "pkg-config"):
        executable = shutil.which(tool)
        if not executable:
            raise RuntimeError("missing build tool: " + tool)
        paths.append(Path(executable).resolve())
    for package in ("libndn-cxx", "libnac-abe", "ndnsd"):
        directory = subprocess.check_output(
            ["pkg-config", "--variable=pcfiledir", package], text=True).strip()
        paths.append(Path(directory) / (package + ".pc"))
    return hashes(paths)


def compiler_dependencies(fresh, source, build):
    """Bind actual selected headers as well as GCC's dependency path lists."""
    records = {}
    input_hashes = {}
    selected = set()
    objects = sorted(fresh.rglob("*.o"))
    if not objects:
        raise RuntimeError("missing compiled objects")
    for obj in objects:
        dep = preflight.require_file(obj.with_suffix(".d"), "COMPILER_DEPENDENCY")
        text = dep.read_text().replace("\\\n", " ")
        dependencies = shlex.split(text.split(":", 1)[1])
        target = Path(text.split(":", 1)[0].strip())
        parents = set(obj.parents) | set(target.parents)
        owner = (ROOT / "pythonWrapper" if fresh / "python-base" in parents
                 else ROOT / "NDNSF-DistributedRepo/pythonWrapper"
                 if fresh / "python-repo" in parents else fresh)
        svs = []
        inputs = []
        for name in dependencies:
            path = (owner / name).resolve()
            inputs.append(path)
            if "ndn-svs" not in path.parts:
                continue
            if not (source in path.parents or build in path.parents):
                raise RuntimeError("compiler selected foreign SVS header: " + str(path))
            svs.append(path)
            selected.add(path)
        for path in set(inputs):
            if str(path) not in input_hashes:
                input_hashes.update(hashes([path]))
        records[str(dep)] = {"sha256": preflight.sha256(dep),
                             "svs_headers": {str(path): input_hashes[str(path)] for path in sorted(set(svs))},
                             "inputs": {str(path): input_hashes[str(path)] for path in sorted(set(inputs))}}
    if not selected:
        raise RuntimeError("no compiled object proves the declared SVS headers")
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--build-dir", default="build-spec202-candidate")
    parser.add_argument("--svs-source", type=Path,
                        default=Path("/usr/local/src/spec192-ndn-svs-9f2d8a4"))
    parser.add_argument("--svs-build", type=Path,
                        default=Path("/usr/local/src/spec192-ndn-svs-9f2d8a4/build"))
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("run the build unprivileged; only the selected install uses sudo -n")
    if Path(args.build_dir).name != args.build_dir or not args.build_dir.startswith(
            "build-spec202-candidate"):
        parser.error("--build-dir must be a fresh build-spec202-candidate* child")
    build = ROOT / args.build_dir
    output = args.output_dir.resolve()
    try:
        output.relative_to(ROOT / "specs/202-distributedrepo-convergence/evidence")
    except ValueError:
        parser.error("build evidence must be retained beneath Spec202/evidence")
    if build.exists() or output.exists():
        parser.error("build and evidence output directories must both be fresh")
    source_sha, source_files = preflight.source_candidate(ROOT)
    external_before = sdk_inputs(args.svs_source.resolve(), args.svs_build.resolve())
    output.mkdir(parents=True)
    previous = [*(ROOT / "build" / name for name in NATIVE_OUTPUTS),
                *(Path("/usr/local/lib") / name for name in INSTALLED_LIBRARIES)]
    for directory, pattern in ((ROOT / "pythonWrapper/ndnsf", "_ndnsf*.so"),
                               (ROOT / "NDNSF-DistributedRepo/pythonWrapper/py_repoclient",
                                "_py_repoclient*.so")):
        previous.extend(directory.glob(pattern))
    previous = [path for path in previous if path.is_file()]
    for path in previous:
        backup = output / "previous-artifacts" / path.relative_to("/")
        backup.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(path, backup)
    (output / "previous-artifacts.json").write_text(json.dumps(hashes(previous), indent=2) + "\n")
    environment = dict(os.environ)
    for name in ("CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "LIBRARY_PATH",
                 "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "PKG_CONFIG_PATH",
                 "PKG_CONFIG_LIBDIR", "NDNSF_RUNTIME_RPATH", "NDNSF_CONTAINER_BUILD",
                 "NDNSF_LIBRARY_DIR", "NDNSF_GLOBAL_NATIVE_DIGESTS",
                 "NDNSF_NDN_SVS_SOURCE_TREE", "NDNSF_NDN_SVS_BUILD_TREE",
                 "LD_LIBRARY_PATH", "LD_PRELOAD", "PYTHONPATH", "PYTHONHOME",
                 "NDNSF_NAC_ABE_PREFIX", "WAFDIR"):
        environment.pop(name, None)
    sdk_source, sdk_build = args.svs_source.resolve(), args.svs_build.resolve()
    effective = preflight.candidate_build_environment(ROOT, build, sdk_source, sdk_build)
    environment.update(effective)
    environment["NDNSF_SKIP_DEV_PIP_INSTALL"] = "1"
    commands = {label: (command, cwd) for label, command, cwd in
                preflight.candidate_build_commands(ROOT, build, sdk_source, sdk_build, sys.executable)}
    records = []

    def run(label):
        command, cwd = commands[label]
        log = output / (label + ".log")
        started = time.monotonic()
        with log.open("wb") as stream:
            result = subprocess.run(command, cwd=str(cwd), env=environment,
                                    stdout=stream, stderr=subprocess.STDOUT)
        records.append({"step": label, "argv": command, "cwd": str(cwd),
                        "environment": dict(effective),
                        "exit_status": result.returncode,
                        "wall_seconds": time.monotonic() - started,
                        "log": str(log), "log_sha256": preflight.sha256(log)})
        print(label, "exit", result.returncode, "log", log, flush=True)
        if result.returncode:
            raise RuntimeError("candidate build failed at " + label)

    run("configure")
    run("native-build")
    fresh_outputs = hashes([build / name for name in NATIVE_OUTPUTS])
    run("native-install")
    if hashes([build / name for name in NATIVE_OUTPUTS]) != fresh_outputs:
        raise RuntimeError("install changed the already built native candidate")
    for name in NATIVE_OUTPUTS:
        destination = ROOT / "build" / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(build / name, destination)
    installed = {name: preflight.sha256(Path("/usr/local/lib") / name)
                 for name in INSTALLED_LIBRARIES}
    for name in INSTALLED_LIBRARIES:
        if installed[name] != preflight.sha256(build / name):
            raise RuntimeError("installed native library differs from fresh output: " + name)
    effective = preflight.candidate_build_environment(ROOT, build, sdk_source, sdk_build, bindings=True)
    environment.update(effective)
    environment["NDNSF_GLOBAL_NATIVE_DIGESTS"] = json.dumps(installed)
    run("base-binding")
    run("repo-binding")
    run("runtime-import")
    dependencies = compiler_dependencies(build, sdk_source, sdk_build)
    loaded = json.loads((output / "runtime-import.log").read_text().splitlines()[-1])
    for name, expected in (("libndn-svs.so", preflight.sha256(args.svs_build / "libndn-svs.so")),
                           *((name, installed[name]) for name in INSTALLED_LIBRARIES)):
        matches = [Path(path) for path in loaded if Path(path).name.startswith(name)]
        if len(matches) != 1 or preflight.sha256(matches[0]) != expected:
            raise RuntimeError("actual Python import loaded a different composition: " + name)
    after_sha, after_files = preflight.source_candidate(ROOT)
    if (source_sha, source_files) != (after_sha, after_files):
        raise RuntimeError("source changed during candidate build; no receipt accepted")
    if external_before != sdk_inputs(args.svs_source.resolve(), args.svs_build.resolve()):
        raise RuntimeError("external SDK changed during candidate build")
    extensions = []
    for directory, pattern in ((ROOT / "pythonWrapper/ndnsf", "_ndnsf*.so"),
                               (ROOT / "NDNSF-DistributedRepo/pythonWrapper/py_repoclient",
                                "_py_repoclient*.so")):
        matches = list(directory.glob(pattern))
        if len(matches) != 1:
            raise RuntimeError("expected exactly one fresh extension: " + str(directory))
        extensions.extend(matches)
    artifacts = hashes([*(ROOT / "build" / name for name in NATIVE_OUTPUTS),
                        *(Path("/usr/local/lib") / name for name in INSTALLED_LIBRARIES),
                        *extensions])
    runtime = hashes([Path(path) for path in loaded])
    for name in NATIVE_OUTPUTS:
        if name.endswith(".a"):
            continue
        runtime.update(preflight.resolved_libraries(ROOT / "build" / name))
    receipt = {"spec": "202", "source_before": source_sha, "source_after": after_sha,
               "source_files": source_files, "steps": records,
               "fresh_outputs": fresh_outputs, "artifacts": artifacts,
               "configuration": hashes([build / "config.hpp", build / "c4che/_cache.py"]),
               "external_inputs": external_before, "runtime": runtime}
    receipt["compiler_dependencies"] = dependencies
    path = ROOT / "build/spec202-source-build-receipt.json"
    path.write_text(json.dumps(receipt, sort_keys=True, indent=2) + "\n")
    print("BUILD_RECEIPT", path, preflight.sha256(path), flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, preflight.PreflightError) as error:
        print("BUILD_REJECT:", error, file=sys.stderr)
        sys.exit(2)
