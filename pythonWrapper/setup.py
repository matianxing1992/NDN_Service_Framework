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


def build_extension() -> Extension:
    import pybind11

    include_dirs, library_dirs, libraries, extra_link_args = pkg_config(
        "libndn-cxx",
        "libndn-svs",
        "libnac-abe",
        "ndnsd",
    )

    local_build = ROOT / "build"
    if local_build.exists():
        library_dirs.insert(0, str(local_build))
        extra_link_args.append(f"-Wl,-rpath,{local_build}")

    env_library_dir = os.environ.get("NDNSF_LIBRARY_DIR")
    if env_library_dir:
        for value in env_library_dir.split(os.pathsep):
            if value:
                library_dirs.insert(0, str(Path(value).expanduser().resolve()))
                extra_link_args.append(f"-Wl,-rpath,{Path(value).expanduser().resolve()}")

    return Extension(
        "ndnsf._ndnsf",
        sources=["src/ndnsf/_ndnsf.cpp"],
        include_dirs=[
            pybind11.get_include(),
            str(ROOT),
            str(ROOT / "ndn-service-framework"),
            *include_dirs,
        ],
        library_dirs=library_dirs,
        libraries=["ndn-service-framework", *libraries],
        extra_compile_args=["-std=c++17"],
        extra_link_args=extra_link_args,
        language="c++",
    )


setup(ext_modules=[build_extension()])
