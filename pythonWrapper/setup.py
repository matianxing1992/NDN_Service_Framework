from __future__ import annotations

import os
import shlex
import subprocess
from pathlib import Path

from setuptools import Extension, setup


ROOT = Path(__file__).resolve().parents[1]


def pkg_config(*packages: str) -> tuple[list[str], list[str], list[str], list[str]]:
    try:
        output = subprocess.check_output(
            ["pkg-config", "--cflags", "--libs", *packages],
            text=True,
        )
    except Exception:
        return [], [], [], []

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
    return source, build


def build_extension() -> Extension:
    import pybind11

    svs_pair = explicit_ndn_svs_pair()
    include_dirs, library_dirs, libraries, extra_link_args = pkg_config(
        "libndn-cxx",
        *([] if svs_pair else ["libndn-svs"]),
        "libnac-abe",
        "ndnsd",
    )

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
    else:
        # Preserve the ordinary editable developer build when no immutable
        # candidate was requested.  Its transitive closure remains the build
        # system's responsibility; setup.py must not guess extra ABI prefixes.
        local_build = ROOT / "build"
        candidate_dirs = [str(local_build)] if local_build.exists() else []

    library_dirs = list(dict.fromkeys([*candidate_dirs, *library_dirs]))
    extra_link_args = [
        *[f"-Wl,-rpath,{value}" for value in candidate_dirs],
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
        extra_link_args.insert(0, f"-Wl,-rpath,{build}")

    return Extension(
        "ndnsf._ndnsf",
        sources=["src/ndnsf/_ndnsf.cpp"],
        include_dirs=[
            *svs_includes,
            pybind11.get_include(),
            str(ROOT),
            str(ROOT / "ndn-service-framework"),
            *include_dirs,
        ],
        library_dirs=library_dirs,
        libraries=["ndn-service-framework", *libraries],
        extra_objects=svs_objects,
        extra_compile_args=["-std=c++17"],
        extra_link_args=extra_link_args,
        language="c++",
    )


setup(ext_modules=[build_extension()])
