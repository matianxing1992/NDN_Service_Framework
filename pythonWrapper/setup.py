from __future__ import annotations

import os
import shlex
import subprocess
from pathlib import Path

from setuptools import Extension, setup


ROOT = Path(__file__).resolve().parents[1]
REPOSITORY_ROOT = ROOT
HISTORICAL_LOCAL_PREFIX = (REPOSITORY_ROOT / ".local-boost171").resolve()
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


def explicit_ndn_svs_pair():
    source = os.environ.get("NDNSF_NDN_SVS_SOURCE_TREE", "")
    build = os.environ.get("NDNSF_NDN_SVS_BUILD_TREE", "")
    if bool(source) != bool(build):
        raise RuntimeError("NDNSF_NDN_SVS_SOURCE_TREE and NDNSF_NDN_SVS_BUILD_TREE must be supplied together")
    if not source:
        return None
    source, build = Path(source).expanduser().resolve(), Path(build).expanduser().resolve()
    for path in (source / "ndn-svs/svspubsub.hpp", build / "config.hpp",
                 build / "libndn-svs.so"):
        if not path.is_file():
            raise RuntimeError("NDNSF_NDN_SVS pair is missing required file: " + str(path))
    reject_non_global_dependency_paths([str(source), str(build)],
                                       "NDNSF_NDN_SVS source/build")
    return source, build


def explicit_nac_abe_prefix():
    value = os.environ.get("NDNSF_NAC_ABE_PREFIX", "")
    if not value:
        return None
    prefix = Path(value).expanduser().resolve()
    for relative in ("include/nac-abe/consumer.hpp", "lib/libnac-abe.so"):
        path = prefix / relative
        if not path.is_file():
            raise RuntimeError("NDNSF_NAC_ABE_PREFIX is missing required file: " + str(path))
    reject_non_global_dependency_paths([str(prefix)], "NDNSF_NAC_ABE_PREFIX")
    return prefix


def build_extension() -> Extension:
    import pybind11

    svs_pair = explicit_ndn_svs_pair()
    nac_prefix = explicit_nac_abe_prefix()
    include_dirs, library_dirs, libraries, extra_link_args = pkg_config(
        "libndn-cxx",
        *([] if svs_pair else ["libndn-svs"]),
        "libnac-abe",
        "ndnsd",
    )
    reject_historical_local_paths([*include_dirs, *library_dirs], "pkg-config")
    reject_non_global_dependency_paths([*include_dirs, *library_dirs], "pkg-config")
    reject_historical_local_link_flags(extra_link_args, "pkg-config")
    reject_non_global_dependency_paths(linker_path_values(extra_link_args),
                                       "pkg-config linker paths")

    env_library_dir = os.environ.get("NDNSF_LIBRARY_DIR")
    if env_library_dir:
        # An explicit candidate is an exclusive closure boundary.  In
        # particular, never append the repository's historical ``build`` or
        # ``.local-boost171`` directories: a same-SONAME libndn-cxx from either
        # location can coexist with the system NFD's /usr/local build and make
        # the first protocol operation fail even though importing succeeds.
        candidate_dirs = [
            str(Path(value).expanduser().resolve())
            for value in env_library_dir.split(os.pathsep)
            if value
        ]
        if not candidate_dirs:
            raise RuntimeError("NDNSF_LIBRARY_DIR contains no library directory")
        reject_historical_local_paths([str(path) for path in candidate_dirs],
                                      "NDNSF_LIBRARY_DIR")
        missing_dirs = [value for value in candidate_dirs if not Path(value).is_dir()]
        if missing_dirs:
            raise RuntimeError(
                "NDNSF_LIBRARY_DIR does not exist: " + ", ".join(missing_dirs)
            )
        if not any(
            any(Path(value).glob("libndn-service-framework.so*"))
            for value in candidate_dirs
        ):
            raise RuntimeError(
                "NDNSF_LIBRARY_DIR does not contain libndn-service-framework"
            )
        if not any(
            any(Path(value).glob("libndnsf-distributed-inference.so*"))
            for value in candidate_dirs
        ):
            raise RuntimeError(
                "NDNSF_LIBRARY_DIR does not contain libndnsf-distributed-inference"
            )
    else:
        # Preserve the ordinary editable developer build when no immutable
        # candidate was requested.  Its transitive closure remains the build
        # system's responsibility; setup.py must not guess extra ABI prefixes.
        local_build = ROOT / "build"
        candidate_dirs = [str(local_build)] if local_build.exists() else []

    library_dirs = list(dict.fromkeys([*candidate_dirs, *library_dirs]))
    runtime_rpath = os.environ.get("NDNSF_RUNTIME_RPATH")
    runtime_dirs = ([value for value in runtime_rpath.split(os.pathsep) if value]
                    if runtime_rpath else candidate_dirs)
    reject_historical_local_paths(runtime_dirs, "NDNSF_RUNTIME_RPATH")
    if runtime_rpath:
        validate_runtime_rpath(runtime_dirs, "NDNSF_RUNTIME_RPATH")
    reject_historical_local_link_flags(
        [f"-Wl,-rpath,{value}" for value in runtime_dirs],
        "NDNSF_RUNTIME_RPATH")
    extra_link_args = [
        *[f"-Wl,-rpath,{value}" for value in runtime_dirs],
        *extra_link_args,
    ]
    svs_includes = []
    svs_objects = []
    if svs_pair:
        source, build = svs_pair
        svs_includes = [str(source), str(build)]
        # Link this exact library, avoiding a same-SONAME installed SVS even
        # when another dependency supplies /usr/local/lib earlier in -L.
        svs_objects = [str(build / "libndn-svs.so")]
        libraries = [name for name in libraries if name != "ndn-svs"]
        if not runtime_rpath:
            extra_link_args.insert(0, f"-Wl,-rpath,{build}")

    nac_includes = []
    nac_objects = []
    if nac_prefix:
        nac_includes = [str(nac_prefix / "include")]
        library_dirs = list(dict.fromkeys([str(nac_prefix / "lib"), *library_dirs]))
        libraries = [name for name in libraries if name != "nac-abe"]
        nac_objects = [str(nac_prefix / "lib/libnac-abe.so")]
        if not runtime_rpath:
            extra_link_args.append(f"-Wl,-rpath,{nac_prefix / 'lib'}")

    return Extension(
        "ndnsf._ndnsf",
        # All DI implementation symbols, including NativeGrantVerifier, come
        # from the single installable native library.  The binding owns only
        # its pybind11 translation units and never compiles a second DI copy.
        sources=[
            "src/ndnsf/_ndnsf.cpp",
            "src/ndnsf/di_bindings.cpp",
        ],
        include_dirs=[
            *nac_includes,
            *svs_includes,
            pybind11.get_include(),
            str(ROOT),
            str(ROOT / "ndn-service-framework"),
            str(ROOT / "NDNSF-DistributedInference/cpp/ndnsf-di"),
            *include_dirs,
        ],
        library_dirs=library_dirs,
        libraries=["ndn-service-framework", "ndnsf-distributed-inference", *libraries],
        extra_objects=[*svs_objects, *nac_objects],
        extra_compile_args=["-std=c++17"],
        extra_link_args=extra_link_args,
        language="c++",
    )


setup(ext_modules=[build_extension()])
