#!/usr/bin/env python3
"""Rebuild and attest the native/binding composition used by the shared runner."""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys
import time

import preflight


ROOT = Path(__file__).resolve().parents[2]
TARGETS = preflight.BUILD_TARGETS
NATIVE_OUTPUTS = preflight.BUILD_OUTPUTS
INSTALLED_BUILD_OUTPUTS = preflight.INSTALLED_BUILD_OUTPUTS


def hashes(paths):
    return {str(path): preflight.sha256(preflight.require_file(path, "BUILD_INPUT"))
            for path in sorted(set(paths))}


def sdk_inputs(source, build, ndn_cxx_source):
    private_headers = ndn_cxx_source / "ndn-cxx"
    runtime = preflight.dependency_prefix("libonnxruntime.so")
    svs_headers = (source / "ndn-svs" if source else
                   preflight.dependency_prefix("libndn-svs.so") / "include/ndn-svs")
    paths = [build / "libndn-svs.so", preflight.DEPENDENCY_IDENTITY,
             private_headers / "security/pib/impl/pib-sqlite3.hpp",
             runtime / "lib/libonnxruntime.so"]
    if source is not None:
        paths.append(source / "wscript")
        config = build / "config.hpp"
        paths.append(config if config.is_file() else svs_headers / "config.hpp")
    for directory in (svs_headers, private_headers, runtime / "include",
                      Path("/usr/local/include/ndn-cxx"),
                      preflight.dependency_prefix("libnac-abe.so") / "include/nac-abe",
                      Path("/usr/local/include/ndnsd"), Path("/usr/local/include/openabe")):
        if not directory.is_dir():
            raise RuntimeError("missing declared SDK headers: " + str(directory))
        paths.extend(path for path in directory.rglob("*")
                     if path.is_file() and (path.suffix in (".h", ".hpp") or
                         (directory == svs_headers and path.suffix == ".cpp")))
    paths.extend(Path(tool).resolve() for tool in preflight.BUILD_TOOLS.values())
    pkg_config_environment = dict(os.environ, PKG_CONFIG_PATH=preflight.pkg_config_path())
    pkg_config_environment.pop("PKG_CONFIG_LIBDIR", None)
    for package in ("libndn-cxx", "libnac-abe", "ndnsd", "libndn-svs", "onnxruntime"):
        directory = subprocess.check_output(
            [preflight.BUILD_TOOLS["PKGCONFIG"], "--variable=pcfiledir", package],
            text=True, env=pkg_config_environment).strip()
        paths.append(Path(directory) / (package + ".pc"))
    return hashes(paths)


def compiler_dependencies(fresh, source, build):
    """Bind actual selected headers as well as GCC's dependency path lists."""
    records = {}
    input_hashes = {}
    selected = set()
    svs_source = source if source is not None else preflight.dependency_prefix("libndn-svs.so") / "include"
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
            if not (svs_source in path.parents or build in path.parents):
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
    parser.add_argument("--svs-source", type=Path, help="override the installed SVS SDK with an explicit pair")
    parser.add_argument("--svs-build", type=Path, help="required with --svs-source")
    parser.add_argument("--ndn-cxx-source", type=Path, required=True,
                        help="NDN-CXX source tree to build against; its exact revision is "
                             "recorded in the receipt and bound by every later verification")
    args = parser.parse_args()
    if os.geteuid() == 0:
        parser.error("run the candidate build and private install unprivileged")
    output = args.output_dir.resolve()
    if output == ROOT or ROOT in output.parents or output.exists():
        parser.error("--output-dir must be a fresh directory outside the checkout")
    build = output / "build"
    libraries = output / "prefix/lib"
    bindings = output / "python"
    if bool(args.svs_source) != bool(args.svs_build):
        parser.error("--svs-source and --svs-build must be supplied together")
    sdk_source = args.svs_source.resolve() if args.svs_source else None
    sdk_build = (args.svs_build.resolve() if args.svs_build else
                 preflight.dependency_prefix("libndn-svs.so") / "lib")
    ndn_cxx_source = args.ndn_cxx_source.resolve()
    # Any revision is admissible as long as it is a clean, identifiable tree: the receipt records the
    # exact commit and every file digest, so a later verification binds what was actually used. Pinning
    # one commit here would force the candidate onto an older ndn-cxx than the host's installation and
    # made the pinned tree the only accepted input.
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ndn_cxx_source,
                                       text=True).strip()
    if len(revision) != 40:
        parser.error("--ndn-cxx-source is not a git checkout with an identifiable revision")
    dirty = subprocess.run(["git", "status", "--porcelain"], cwd=ndn_cxx_source,
                           text=True, capture_output=True, check=True).stdout.strip()
    if dirty:
        parser.error("--ndn-cxx-source has uncommitted changes; record a clean revision")
    source_sha, source_files = preflight.source_candidate(ROOT)
    external_before = sdk_inputs(sdk_source, sdk_build, ndn_cxx_source)
    output.mkdir(parents=True)
    environment = dict(os.environ)
    for name in ("CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS", "LIBRARY_PATH",
                 "CPATH", "C_INCLUDE_PATH", "CPLUS_INCLUDE_PATH", "PKG_CONFIG_PATH",
                 "PKG_CONFIG_LIBDIR", "NDNSF_RUNTIME_RPATH", "NDNSF_CONTAINER_BUILD",
                 "NDNSF_RUNTIME_PREFIX", "NDNSF_LIBRARY_DIR", "NDNSF_GLOBAL_NATIVE_DIGESTS",
                 "NDNSF_NDN_SVS_SOURCE_TREE", "NDNSF_NDN_SVS_BUILD_TREE",
                 "LD_LIBRARY_PATH", "LD_PRELOAD", "LDSHARED", "PYTHONPATH", "PYTHONHOME",
                 "NDNSF_NAC_ABE_PREFIX", "WAFDIR", "NO_LOCK_IN_OUT"):
        environment.pop(name, None)
    effective = preflight.candidate_build_environment(
        build, sdk_source, sdk_build, ndn_cxx_source)
    environment.update(effective)
    subprocess.run([sys.executable, "-c", "import pybind11, setuptools"],
                   env=environment, check=True)
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
    installed = {name: preflight.sha256(libraries / name)
                 for name in INSTALLED_BUILD_OUTPUTS}
    for name, relative in INSTALLED_BUILD_OUTPUTS.items():
        if installed[name] != preflight.sha256(build / relative):
            raise RuntimeError("installed native library differs from fresh output: " + name)
    effective = preflight.candidate_build_environment(
        build, sdk_source, sdk_build, ndn_cxx_source, bindings=True)
    environment.update(effective)
    run("base-binding")
    run("repo-binding")
    run("runtime-import")
    dependencies = compiler_dependencies(build, sdk_source, sdk_build)
    loaded = json.loads((output / "runtime-import.log").read_text().splitlines()[-1])
    for name, expected in (("libndn-svs.so", preflight.sha256(sdk_build / "libndn-svs.so")),
                           *((name, installed[name]) for name in INSTALLED_BUILD_OUTPUTS
                             if name.endswith(".so"))):
        matches = [Path(path) for path in loaded if Path(path).name.startswith(name)]
        if len(matches) != 1 or preflight.sha256(matches[0]) != expected:
            raise RuntimeError("actual Python import loaded a different composition: " + name)
    after_sha, after_files = preflight.source_candidate(ROOT)
    if (source_sha, source_files) != (after_sha, after_files):
        raise RuntimeError("source changed during candidate build; no receipt accepted")
    if external_before != sdk_inputs(sdk_source, sdk_build, ndn_cxx_source):
        raise RuntimeError("external SDK changed during candidate build")
    extensions = []
    for directory, pattern in ((bindings / "ndnsf", "_ndnsf*.so"),
                               (bindings / "py_repoclient",
                                "_py_repoclient*.so")):
        matches = list(directory.glob(pattern))
        if len(matches) != 1:
            raise RuntimeError("expected exactly one fresh extension: " + str(directory))
        extensions.extend(matches)
    artifacts = hashes([*(build / name for name in NATIVE_OUTPUTS),
                        *(libraries / name for name in INSTALLED_BUILD_OUTPUTS),
                        *extensions])
    runtime = hashes([Path(path) for path in loaded])
    if any(str(path.resolve()) not in runtime for path in extensions):
        raise RuntimeError("runtime import did not load both private Python extensions")
    for name in NATIVE_OUTPUTS:
        if name.endswith(".a"):
            continue
        runtime.update(preflight.resolved_libraries(build / name, effective["LD_LIBRARY_PATH"]))
    receipt = {"spec": "202", "source_before": source_sha, "source_after": after_sha,
               "source_files": source_files, "steps": records,
               "fresh_outputs": fresh_outputs, "artifacts": artifacts,
               "configuration": hashes([build / "config.hpp", build / "c4che/_cache.py"]),
               "external_inputs": external_before, "runtime": runtime}
    receipt["compiler_dependencies"] = dependencies
    output_receipt = output / "spec202-source-build-receipt.json"
    output_receipt.write_text(json.dumps(receipt, sort_keys=True, indent=2) + "\n")
    print("BUILD_RECEIPT", output_receipt, preflight.sha256(output_receipt), flush=True)
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print("BUILD_REJECT:", error, file=sys.stderr)
        sys.exit(2)
