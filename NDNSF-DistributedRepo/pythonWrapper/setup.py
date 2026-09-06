from __future__ import annotations

import os
import shlex
import subprocess
from pathlib import Path

from setuptools import Extension, find_packages, setup


ROOT = Path(__file__).resolve().parents[2]
WRAPPER = Path(__file__).resolve().parent


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
        explicit_includes.extend([str(source), str(build)])
        extra_objects.append(str(build / "libndn-svs.so"))
    nac = os.environ.get("NDNSF_NAC_ABE_PREFIX", "")
    if nac:
        nac = Path(nac).resolve()
        for relative in ("include/nac-abe/consumer.hpp", "lib/libnac-abe.so"):
            if not (nac / relative).is_file():
                raise RuntimeError("NDNSF_NAC_ABE_PREFIX is missing required file: " + str(nac / relative))
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
        for path in candidate_dirs:
            if not path.is_dir():
                raise RuntimeError("NDNSF_LIBRARY_DIR does not exist: " + str(path))
            if not (path / "libndn-service-framework.so").is_file():
                raise RuntimeError("NDNSF_LIBRARY_DIR does not contain libndn-service-framework: " + str(path))
            library_dirs.insert(0, str(path))
            extra_link_args.append(f"-Wl,-rpath,{path}")
    if source:
        extra_link_args.append(f"-Wl,-rpath,{build}")
    if nac:
        library_dirs.insert(0, str(nac / "lib"))
        libraries = [name for name in libraries if name != "nac-abe"]
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
