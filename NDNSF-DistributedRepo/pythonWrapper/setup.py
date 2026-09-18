from __future__ import annotations

import os
import shlex
import subprocess
from pathlib import Path

from setuptools import Extension, find_packages, setup


ROOT = Path(__file__).resolve().parents[2]
WRAPPER = Path(__file__).resolve().parent
HISTORICAL_LOCAL_PREFIX = (ROOT / ".local-boost171").resolve()
HOST_GLOBAL_DEPENDENCY_ROOTS = tuple(
    Path(value).resolve()
    for value in (
        "/usr",
        "/usr/local",
        "/opt/onnxruntime-1.26.0",
    )
)
CONTAINER_GLOBAL_DEPENDENCY_ROOTS = tuple(
    Path(value).resolve()
    for value in ("/opt/ndn-base", "/opt/onnx", "/opt/ndnsf-stage", "/opt/ndnsf-di")
)


def dependency_roots() -> tuple[Path, ...]:
    roots = list(HOST_GLOBAL_DEPENDENCY_ROOTS)
    if os.environ.get("NDNSF_CONTAINER_BUILD") == "1":
        roots.extend(CONTAINER_GLOBAL_DEPENDENCY_ROOTS)
    return tuple(roots)


def reject_historical_local_paths(paths: list[str], owner: str) -> None:
    """Fail closed when host binding discovery selects the retired tree."""
    for value in paths:
        if not value:
            continue
        path = Path(value).expanduser().resolve()
        if path == HISTORICAL_LOCAL_PREFIX or HISTORICAL_LOCAL_PREFIX in path.parents:
            raise RuntimeError(
                f"{owner} resolved to retired {HISTORICAL_LOCAL_PREFIX}; "
                "use the canonical host NDN-CXX/NFD prefix or a complete "
                "isolated runtime closure"
            )


def reject_historical_local_link_flags(flags: list[str], owner: str) -> None:
    """Reject RPATH-like linker flags that name the retired tree."""
    historical = str(HISTORICAL_LOCAL_PREFIX)
    path_values = []
    for flag in flags:
        value = str(flag)
        if historical in value or ".local-boost171/" in value:
            raise RuntimeError(
                f"{owner} resolved to retired {HISTORICAL_LOCAL_PREFIX}; "
                "use the canonical host NDN-CXX/NFD prefix or a complete "
                "isolated runtime closure"
            )
        if not value.startswith("-Wl,"):
            continue
        parts = value[4:].split(",")
        for index, part in enumerate(parts):
            if part in ("-rpath", "-rpath-link", "-R") and index + 1 < len(parts):
                path_values.append(parts[index + 1])
            elif part.startswith("-rpath="):
                path_values.append(part.split("=", 1)[1])
            elif part.startswith("-R") and part != "-R":
                path_values.append(part[2:])
    reject_historical_local_paths(path_values, owner)


def reject_non_global_dependency_paths(paths: list[str], owner: str) -> None:
    """Reject external dependency paths outside the declared global roots."""
    for value in paths:
        if not value:
            continue
        if str(value).startswith("$ORIGIN"):
            raise RuntimeError(
                f"{owner} contains $ORIGIN; container-only runtime paths must not "
                "enter host dependency discovery"
            )
        path = Path(value).expanduser().resolve()
        roots = dependency_roots()
        if not any(path == root or root in path.parents for root in roots):
            root_list = ", ".join(str(root) for root in roots)
            raise RuntimeError(
                f"{owner} resolved to undeclared dependency root {path}; "
                f"allowed global roots: {root_list}"
            )


def linker_path_values(flags: list[str]) -> list[str]:
    values: list[str] = []
    for flag in flags:
        if not flag.startswith("-Wl,"):
            continue
        parts = flag[4:].split(",")
        for index, part in enumerate(parts):
            if part in ("-rpath", "-rpath-link", "-R") and index + 1 < len(parts):
                values.append(parts[index + 1])
            elif part.startswith("-rpath="):
                values.append(part.split("=", 1)[1])
            elif part.startswith("-R") and part != "-R":
                values.append(part[2:])
    return values


def validate_pkg_config_environment() -> None:
    for variable in ("PKG_CONFIG_PATH", "PKG_CONFIG_LIBDIR"):
        value = os.environ.get(variable, "")
        if not value:
            continue
        paths = [Path(item).expanduser().resolve()
                 for item in value.split(os.pathsep) if item]
        reject_historical_local_paths([str(path) for path in paths], variable)
        reject_non_global_dependency_paths([str(path) for path in paths], variable)
        missing = [str(path) for path in paths if not path.is_dir()]
        if missing:
            raise RuntimeError(
                f"{variable} contains missing directories: " + ", ".join(missing))


def validate_runtime_rpath(values: list[str], owner: str) -> None:
    origins = [value for value in values
               if value == "$ORIGIN" or value.startswith("$ORIGIN/")]
    concrete = [value for value in values if value not in origins]
    if origins and os.environ.get("NDNSF_CONTAINER_BUILD") != "1":
        raise RuntimeError(
            f"{owner} contains $ORIGIN; container-only runtime paths require "
            "NDNSF_CONTAINER_BUILD=1")
    if origins and not any(
            value == "/opt/ndn-base" or value.startswith("/opt/ndn-base/")
            for value in concrete):
        raise RuntimeError(
            f"{owner} with $ORIGIN must also name the declared /opt/ndn-base "
            "SDK root")
    reject_non_global_dependency_paths(concrete, owner)


def pkg_config(*packages: str) -> tuple[list[str], list[str], list[str], list[str]]:
    validate_pkg_config_environment()
    try:
        output = subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", *packages],
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise RuntimeError(
            "pkg-config failed for " + ", ".join(packages)) from error

    include_dirs: list[str] = []
    library_dirs: list[str] = []
    libraries: list[str] = []
    extra_link_args: list[str] = []
    for token in shlex.split(output):
        if token.startswith("-I"):
            include_dirs.append(token[2:])
        elif token.startswith("-L"):
            library_dirs.append(token[2:])
        elif token.startswith("-l"):
            libraries.append(token[2:])
        else:
            extra_link_args.append(token)
    return include_dirs, library_dirs, libraries, extra_link_args


def build_extension() -> Extension:
    import pybind11

    source = os.environ.get("NDNSF_NDN_SVS_SOURCE_TREE", "")
    build = os.environ.get("NDNSF_NDN_SVS_BUILD_TREE", "")
    if bool(source) != bool(build):
        raise RuntimeError("NDNSF_NDN_SVS source and build must be supplied together")
    explicit_includes, extra_objects = [], []
    if source:
        source, build = Path(source).resolve(), Path(build).resolve()
        for path in (source / "ndn-svs/svspubsub.hpp", build / "config.hpp",
                     build / "libndn-svs.so"):
            if not path.is_file():
                raise RuntimeError("NDNSF_NDN_SVS pair is missing required file: " + str(path))
        reject_non_global_dependency_paths([str(source), str(build)],
                                           "NDNSF_NDN_SVS source/build")
        explicit_includes.extend([str(source), str(build)])
        extra_objects.append(str(build / "libndn-svs.so"))
    nac = os.environ.get("NDNSF_NAC_ABE_PREFIX", "")
    if nac:
        nac = Path(nac).resolve()
        for relative in ("include/nac-abe/consumer.hpp", "lib/libnac-abe.so"):
            if not (nac / relative).is_file():
                raise RuntimeError("NDNSF_NAC_ABE_PREFIX is missing required file: " + str(nac / relative))
        reject_non_global_dependency_paths([str(nac)], "NDNSF_NAC_ABE_PREFIX")
        explicit_includes.insert(0, str(nac / "include"))
        extra_objects.append(str(nac / "lib/libnac-abe.so"))

    # RepoClient's public headers include ServiceUser, whose public surface in
    # turn includes NDN-SVS headers.  Ask pkg-config for both transitive public
    # dependencies instead of relying on an image-specific include path.
    include_dirs, library_dirs, libraries, extra_link_args = pkg_config(
        "libndn-cxx",
        *([] if source else ["libndn-svs"]),
        "libnac-abe",
    )
    reject_historical_local_paths([*include_dirs, *library_dirs], "pkg-config")
    reject_non_global_dependency_paths([*include_dirs, *library_dirs], "pkg-config")
    reject_historical_local_link_flags(extra_link_args, "pkg-config")
    reject_non_global_dependency_paths(linker_path_values(extra_link_args),
                                       "pkg-config linker paths")

    env_library_dir = os.environ.get("NDNSF_LIBRARY_DIR")
    local_build = ROOT / "build"
    if not env_library_dir and local_build.exists():
        library_dirs.insert(0, str(local_build))
        extra_link_args.append(f"-Wl,-rpath,{local_build}")

    if env_library_dir:
        candidate_dirs = [
            Path(value).expanduser().resolve()
            for value in env_library_dir.split(os.pathsep)
            if value
        ]
        if not candidate_dirs:
            raise RuntimeError("NDNSF_LIBRARY_DIR contains no library directory")
        reject_historical_local_paths([str(path) for path in candidate_dirs],
                                      "NDNSF_LIBRARY_DIR")
        for path in candidate_dirs:
            if not path.is_dir():
                raise RuntimeError("NDNSF_LIBRARY_DIR does not exist: " + str(path))
            if not (path / "libndn-service-framework.so").is_file():
                raise RuntimeError("NDNSF_LIBRARY_DIR does not contain libndn-service-framework: " + str(path))
            library_dirs.insert(0, str(path))
            extra_link_args.append(f"-Wl,-rpath,{path}")
    runtime_rpath = os.environ.get("NDNSF_RUNTIME_RPATH")
    if runtime_rpath:
        runtime_dirs = [value for value in runtime_rpath.split(os.pathsep) if value]
        reject_historical_local_paths(runtime_dirs, "NDNSF_RUNTIME_RPATH")
        validate_runtime_rpath(runtime_dirs, "NDNSF_RUNTIME_RPATH")
        reject_historical_local_link_flags(
            [f"-Wl,-rpath,{value}" for value in runtime_dirs],
            "NDNSF_RUNTIME_RPATH")
        extra_link_args = [
            *[f"-Wl,-rpath,{value}" for value in runtime_dirs],
            *[value for value in extra_link_args
              if not value.startswith("-Wl,-rpath,")],
        ]
    elif source:
        extra_link_args.append(f"-Wl,-rpath,{build}")
    if nac:
        library_dirs.insert(0, str(nac / "lib"))
        libraries = [name for name in libraries if name != "nac-abe"]
        if not runtime_rpath:
            extra_link_args.append(f"-Wl,-rpath,{nac / 'lib'}")

    return Extension(
        "py_repoclient._py_repoclient",
        sources=[
            "src/py_repoclient/_py_repoclient.cpp",
            "../src/ArtifactManifest.cpp",
            "../src/ArtifactTransfer.cpp",
            "../src/ArtifactTypes.cpp",
            "../src/backends/FilesystemArtifactStore.cpp",
            "../src/RepoClient.cpp",
            "../src/RepoCore.cpp",
            "../src/RepoNode.cpp",
            "../src/RepoProtocol.cpp",
            "../src/RepoStoreBackend.cpp",
            "../src/RepoTypes.cpp",
        ],
        include_dirs=[
            *explicit_includes,
            pybind11.get_include(),
            str(ROOT),
            str(ROOT / "NDNSF-DistributedRepo/include"),
            str(ROOT / "ndn-service-framework"),
            *include_dirs,
        ],
        library_dirs=library_dirs,
        libraries=["ndn-service-framework", *libraries],
        extra_objects=extra_objects,
        extra_compile_args=["-std=c++17"],
        extra_link_args=extra_link_args,
        language="c++",
    )


setup(
    name="py-repoclient",
    version="0.2.0",
    description=(
        "Public trusted-artifact API and Python bindings for "
        "NDNSF-DistributedRepo"
    ),
    packages=find_packages(include=("py_repoclient", "py_repoclient.*")),
    package_data={"py_repoclient": ["py.typed"]},
    python_requires=">=3.8",
    ext_modules=[build_extension()],
)
