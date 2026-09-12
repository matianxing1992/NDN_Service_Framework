#!/usr/bin/env python3
"""Build and verify the HOST LOCAL Spec180 native runtime (Linux, Python 3.8+).

Use the same Python, cwd and environment as the later LOCAL tests, for example::

    export PYTHONPATH="$PWD/pythonWrapper${PYTHONPATH:+:$PYTHONPATH}"
    python3 scripts/spec180_native_build.py build --build-dir build-system-j2
    python3 scripts/spec180_native_build.py verify --build-dir build-system-j2

The Waf directory must already be configured. This tool does not configure,
install, run a Provider/model, or qualify SIF/Tiger artifacts. Waf and setup.py
remain the build authorities. Commands run sequentially; a lock prevents two
instances of this entry point from building/verifying concurrently.

The manifest binds source/configuration bytes, both native outputs, the actual
imported extension and libraries mapped by a fresh Python process. Extension
mtime is deliberately not evidence of the Controller implementation: that code
lives in libndn-service-framework. Provider dependencies are resolved with ldd
without invoking its main(). verify never repairs PYTHONPATH/LD_LIBRARY_PATH
to hide an incorrect runtime. Run it immediately before tests in their launch
environment; it cannot attest a different/already-running process or prevent
changes made after verification. This is a local regression guard, not a signed
candidate, an external-header/toolchain attestation, or a qualification result.
"""

from __future__ import annotations

import argparse
import ast
from contextlib import contextmanager
import fcntl
import hashlib
import importlib
import importlib.machinery
import json
import os
from pathlib import Path
import pwd
import re
import shlex
import shutil
import subprocess
import sys
import sysconfig
import tempfile


SCHEMA = "spec180-host-local-native-v1"
# The binding links the native DI shared library as well as the framework.
# The process qualification and cold ONNX path also consume the native
# requester, authority and assembly worker.  Listing only the framework and
# Provider lets a fresh Waf output pass while silently omitting those runtime
# artifacts; an older output tree can hide that omission.
TARGETS = ("ndn-service-framework,ndnsf-distributed-inference,"
           "DI_NativeRequester,DI_NativeArtifactAuthority,"
           "di-native-assembly-worker,di-native-provider")
LIBRARY = "libndn-service-framework.so"
SVS_LIBRARY = "libndn-svs.so"
SVS_SOURCE_ENV = "NDNSF_NDN_SVS_SOURCE_TREE"
SVS_BUILD_ENV = "NDNSF_NDN_SVS_BUILD_TREE"
DEFAULT_MANIFEST = "build-system-j2/spec180-native-build.json"
MANIFEST_ENV = "SPEC180_NATIVE_BUILD_MANIFEST"
SOURCE_TREES = (
    "ndn-service-framework", "NDNSF-DistributedInference/cpp",
    "NDNSF-DistributedRepo/include", "pythonWrapper/src", "pythonWrapper/ndnsf",
    ".waf-tools",
)
SOURCE_SUFFIXES = {".cpp", ".cc", ".cxx", ".c", ".hpp", ".h", ".ipp",
                   ".inl", ".inc", ".tcc", ".proto", ".py"}
SOURCE_FILES = (
    "waf", "wscript", "examples/wscript", "tests/wscript",
    "examples/DI_NativeProviderExecutable.cpp",
    "pythonWrapper/setup.py", "pythonWrapper/pyproject.toml",
    "scripts/spec180_native_build.py",
)
# This source is linked into the standalone Provider executable only.  A
# change here must rebuild that executable, but it cannot alter the Python
# extension's ABI or its linked Core/DI shared libraries.  Keep the binding
# reuse decision aligned with the affected-target build policy.
BINDING_INDEPENDENT_SOURCES = frozenset({
    "examples/DI_NativeProviderExecutable.cpp",
    "scripts/spec180_native_build.py",
})
# A change confined to a native implementation translation unit changes the
# shared library that the extension loads, but not the extension's compile
# time ABI.  Headers, build scripts and binding sources remain in the normal
# fingerprint set so an ABI or build-contract change still forces setup.py.
BINDING_IMPLEMENTATION_ROOTS = (
    "ndn-service-framework/",
    "NDNSF-DistributedInference/cpp/",
)
BINDING_IMPLEMENTATION_SUFFIXES = frozenset({".cpp", ".cc", ".cxx", ".c"})
BINDING_RUNTIME_LIBRARY_PREFIXES = (LIBRARY, "libndnsf-distributed-inference.so")
CONFIG_FILES = ("config.hpp", "c4che/_cache.py", "c4che/build.config.py")
SETUP_TOOLCHAIN_ROOT = Path("/usr/bin")
SETUP_FLAG_NAMES = ("CFLAGS", "CXXFLAGS", "CPPFLAGS", "LDFLAGS")


class IdentityError(RuntimeError):
    """Fail-closed local build/runtime identity mismatch."""


def digest(path):
    h = hashlib.sha256()
    with Path(path).open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def file_identity(path):
    path = Path(path).absolute()
    return {"path": str(path), "realpath": str(path.resolve(strict=True)),
            "sha256": digest(path)}


def source_fingerprints(root):
    """Inventory additions/deletions too, including untracked native sources."""
    paths = {root / name for name in SOURCE_FILES}
    for name in SOURCE_TREES:
        paths.update(p for p in (root / name).rglob("*")
                     if p.is_file() and p.suffix in SOURCE_SUFFIXES
                     and "__pycache__" not in p.parts)
    return {str(p.relative_to(root)): file_identity(p) for p in sorted(paths)}


def config_fingerprints(build_dir):
    return {name: file_identity(build_dir / name) for name in CONFIG_FILES}


def waf_tool_identity(root, env):
    """Bind the maintained launcher's selection without importing Waf.

    The configured tree already has an extracted/installed waflib. Match the
    launcher's WAFDIR, install-prefix, local-tree order, then pass the selected
    directory explicitly to its build child. Python caches are generated, not
    build inputs; source and resource file additions/deletions are inventoried.
    """
    launcher = root / "waf"
    header = launcher.read_bytes().split(b"\n#==>\n", 1)[0].decode("latin-1")
    values = {}
    for name in ("VERSION", "REVISION", "INSTALL"):
        matches = re.findall(r"^" + name + r"\s*=\s*(.+)$", header, re.MULTILINE)
        if len(matches) != 1:
            raise IdentityError("WAF_LAUNCHER_CONSTANT_REQUIRED: " + name)
        try:
            value = ast.literal_eval(matches[0])
        except (ValueError, SyntaxError) as error:
            raise IdentityError("WAF_LAUNCHER_CONSTANT_INVALID: " + name) from error
        if not isinstance(value, str) or (name != "INSTALL" and (not value or "/" in value)):
            raise IdentityError("WAF_LAUNCHER_CONSTANT_INVALID: " + name)
        values[name] = value
    if header.splitlines()[0] != "#!/usr/bin/env python3":
        raise IdentityError("WAF_INTERPRETER_CONTRACT_CHANGED")
    search_path = os.pathsep.join(str(root / part)
                                  for part in env.get("PATH", os.defpath).split(os.pathsep))
    python = shutil.which("python3", path=search_path)
    if python is None:
        raise IdentityError("WAF_PYTHON_MISSING")
    if not Path(python).is_absolute():
        python = str(root / python)
    dirname = "waf3-" + values["VERSION"] + "-" + values["REVISION"]
    candidates = [root / env.get("WAFDIR", "")]
    candidates.extend(root / (prefix + "/lib/" + dirname)
                      for prefix in (values["INSTALL"], "/usr", "/usr/local", "/opt"))
    candidates.append(root / ("." + dirname))
    selected = next((path.absolute() for path in candidates
                     if (path / "waflib").exists()), None)
    if selected is None or not (selected / "waflib").is_dir():
        raise IdentityError("WAF_LIBRARY_MISSING: configure the maintained Waf tree first")
    files = {}
    for path in sorted((selected / "waflib").rglob("*")):
        if "__pycache__" in path.relative_to(selected).parts or path.suffix in (".pyc", ".pyo"):
            continue
        if path.is_symlink() and path.is_dir():
            raise IdentityError("WAF_LIBRARY_DIRECTORY_SYMLINK: " + str(path))
        if path.is_file() or path.is_symlink():
            files[str(path.relative_to(selected))] = file_identity(path)
    return {"directory": str(selected), "realpath": str(selected.resolve(strict=True)),
            "launcher": file_identity(launcher), "python": file_identity(python),
            "files": files}


def setup_toolchain_environment():
    prefix = str(SETUP_TOOLCHAIN_ROOT)
    return {"CC": prefix + "/gcc -B" + prefix,
            "CXX": prefix + "/g++ -B" + prefix,
            "LDSHARED": prefix + "/g++ -B" + prefix + " -shared"}


def setup_build_flags(env):
    # None means unset: retain setuptools defaults, never invent optimization
    # flags. These describe the build, not the later runtime environment.
    return {name: env.get(name) for name in SETUP_FLAG_NAMES}


def setup_toolchain_identity(root, env):
    """Pin setup's drivers and check their linker selection without compiling.

    An absolute gcc/g++ alone does not pin ld: this host can otherwise find
    Linuxbrew binutils. Record the exact child-only overrides, real driver and
    linker paths/hashes, and the resolution reported with the same -B flags.
    """
    overrides = setup_toolchain_environment()
    expected = file_identity(SETUP_TOOLCHAIN_ROOT / "ld")
    if Path(expected["realpath"]).parent != SETUP_TOOLCHAIN_ROOT.resolve():
        raise IdentityError("SETUP_LINKER_OUTSIDE_SYSTEM_TOOLCHAIN")
    drivers = {}
    for name, command in overrides.items():
        argv = shlex.split(command)
        driver = file_identity(argv[0])
        if Path(driver["realpath"]).parent != SETUP_TOOLCHAIN_ROOT.resolve():
            raise IdentityError("SETUP_DRIVER_OUTSIDE_SYSTEM_TOOLCHAIN: " + name)
        output = run(argv + ["-print-prog-name=ld"], cwd=root,
                     env=dict(env, **overrides), capture=True, timeout=10).strip()
        if not Path(output).is_absolute():
            raise IdentityError("SETUP_LINKER_NOT_ABSOLUTE: " + output)
        linker = file_identity(output)
        if linker["realpath"] != expected["realpath"] or linker["sha256"] != expected["sha256"]:
            raise IdentityError("WRONG_SETUP_LINKER: " + output)
        drivers[name] = {"argv": argv, "driver": driver, "linker": linker}
    return {"environment": overrides, "drivers": drivers, "linker": expected}


def svs_identity(build_dir):
    """Read only the literal Waf pair, never execute its Python-like cache.

    Bind all public SVS headers, the generated configuration and exact linked
    library. Ambient setup-only overrides cannot replace Waf's selected pair.
    """
    values = {}
    for line in (build_dir / "c4che/_cache.py").read_text().splitlines():
        match = re.fullmatch(r"(NDNSF_NDN_SVS_(?:SOURCE|BUILD)_TREE)\s*=\s*(.+)", line)
        if match:
            if match[1] in values:
                raise IdentityError("DUPLICATE_WAF_SVS_PAIR")
            try:
                values[match[1]] = ast.literal_eval(match[2])
            except (ValueError, SyntaxError) as error:
                raise IdentityError("WAF_SVS_PAIR_INVALID_LITERAL") from error
    if any(not isinstance(values.get(name), str) or not values[name]
           for name in (SVS_SOURCE_ENV, SVS_BUILD_ENV)):
        raise IdentityError("WAF_SVS_PAIR_REQUIRED")
    if any(not Path(value).is_absolute() for value in values.values()):
        raise IdentityError("WAF_SVS_PAIR_NOT_ABSOLUTE")
    source = Path(values[SVS_SOURCE_ENV]).resolve(strict=True)
    build_tree = Path(values[SVS_BUILD_ENV]).resolve(strict=True)
    if not (source / "ndn-svs/svspubsub.hpp").is_file():
        raise IdentityError("WAF_SVS_HEADER_MISSING")
    headers = {str(p.relative_to(source)): file_identity(p)
               for p in sorted((source / "ndn-svs").rglob("*"))
               if p.is_file() and p.suffix in {".hpp", ".h", ".ipp", ".inl", ".tcc"}}
    return {"source_tree": str(source), "build_tree": str(build_tree),
            "headers": headers, "configuration": file_identity(build_tree / "config.hpp"),
            "library": file_identity(build_tree / SVS_LIBRARY)}


def validate_svs_mapping(svs, libraries, code):
    mapped = [value for value in libraries.values()
              if Path(value["realpath"]).name.startswith(SVS_LIBRARY)]
    expected = svs["library"]
    if (len(mapped) != 1 or mapped[0]["realpath"] != expected["realpath"]
            or mapped[0]["sha256"] != expected["sha256"]):
        raise IdentityError(code)


def check_equal(actual, expected, code):
    if actual != expected:
        raise IdentityError(code)


def run(command, *, cwd, env, capture=False, timeout=None):
    """Single subprocess seam; tests replace this, never invoke compilers."""
    result = subprocess.run(command, cwd=str(cwd), env=env, check=True,
                            text=True, stdout=subprocess.PIPE if capture else None,
                            timeout=timeout)
    return result.stdout if capture else None


def mapped_shared_libraries(maps):
    """Check map inode/device as well as path, rejecting replaced/deleted DSOs."""
    libraries = {}
    for line in maps.splitlines():
        fields = line.split(None, 5)
        if len(fields) != 6 or not fields[5].startswith("/"):
            continue
        name = re.sub(r"\\([0-7]{3})", lambda m: chr(int(m[1], 8)), fields[5])
        if ".so" not in Path(name).name:
            continue
        if name.endswith(" (deleted)"):
            raise IdentityError("MAPPED_LIBRARY_DELETED: " + name)
        path = Path(name)
        st = path.stat()
        major, minor = (int(value, 16) for value in fields[3].split(":"))
        if (st.st_ino, os.major(st.st_dev), os.minor(st.st_dev)) != (
                int(fields[4]), major, minor):
            raise IdentityError("MAPPED_LIBRARY_REPLACED: " + name)
        resolved = str(path.resolve(strict=True))
        if resolved not in libraries:
            libraries[resolved] = file_identity(path)
    return dict(sorted(libraries.items()))


def emit_runtime_probe():
    # With node-scoped HOME, Python may load NSS lazily during uid lookup.
    # Exercise that same lookup in every probe before comparing the complete
    # mapping set; do not drop library identities to hide a difference.
    pwd.getpwuid(os.getuid())
    module = importlib.import_module("ndnsf._ndnsf")
    path = Path(module.__file__).absolute()
    if not any(str(path).endswith(suffix)
               for suffix in importlib.machinery.EXTENSION_SUFFIXES):
        raise IdentityError("NOT_A_NATIVE_EXTENSION: " + str(path))
    result = {
        "python": {"executable": file_identity(sys.executable),
                   "prefix": sys.prefix, "version": sys.version,
                   "soabi": sysconfig.get_config_var("SOABI"),
                   "ext_suffix": sysconfig.get_config_var("EXT_SUFFIX")},
        "extension": file_identity(path),
        "mapped_libraries": mapped_shared_libraries(Path("/proc/self/maps").read_text()),
    }
    print("SPEC180_NATIVE_PROBE=" + json.dumps(result, sort_keys=True))


def probe_runtime(python, root, env):
    # -c preserves cwd-based sys.path semantics. Do not prepend pythonWrapper or
    # the selected library dir: a shadowed import/link must fail verification.
    code = ("import runpy,sys; "
            "runpy.run_path(sys.argv[1])['emit_runtime_probe']()")
    # NDN-SVS constructs its default KeyChain at DSO load, before Python can
    # isolate it. Set private locators before exec; never inspect/repair the
    # operator PIB/TPM as a side effect of a read-only build-identity check.
    # Import/loader paths and interpreter selection are left untouched.
    with tempfile.TemporaryDirectory(prefix="spec180-native-probe-") as private:
        probe_env = dict(env, NDN_CLIENT_PIB="pib-sqlite3:" + private + "/pib",
                         NDN_CLIENT_TPM="tpm-file:" + private + "/tpm",
                         NDN_CLIENT_TRANSPORT="unix://" + private + "/unused.sock")
        output = run([python, "-c", code, str(Path(__file__).resolve())],
                     cwd=root, env=probe_env, capture=True, timeout=60)
    records = [line.partition("=")[2] for line in output.splitlines()
               if line.startswith("SPEC180_NATIVE_PROBE=")]
    if len(records) != 1:
        raise IdentityError("RUNTIME_PROBE_INVALID")
    return json.loads(records[0])


def validate_runtime(runtime, root, build_dir):
    expected = root / "pythonWrapper/ndnsf" / (
        "_ndnsf" + runtime["python"]["ext_suffix"])
    # Check lexical path and resolved path: symlinks into another build are
    # not accepted as an in-tree extension or framework library.
    extension = runtime["extension"]
    if (extension["path"] != str(expected)
            or Path(extension["realpath"]).parent != expected.parent):
        raise IdentityError("WRONG_IMPORTED_EXTENSION: " + extension["path"])
    check_equal(extension, file_identity(expected), "EXTENSION_CHANGED_DURING_IMPORT")
    framework = [value for path, value in runtime["mapped_libraries"].items()
                 if Path(path).name.startswith(LIBRARY)]
    built = file_identity(build_dir / LIBRARY)
    if Path(built["realpath"]).parent != build_dir:
        raise IdentityError("FRAMEWORK_OUTPUT_OUTSIDE_BUILD_DIR")
    if len(framework) != 1 or framework[0]["realpath"] != built["realpath"]:
        raise IdentityError("WRONG_FRAMEWORK_LIBRARY: " +
                            ", ".join(lib["realpath"] for lib in framework))
    check_equal(framework[0]["sha256"], built["sha256"],
                "FRAMEWORK_CHANGED_DURING_IMPORT")


def parse_ldd(output):
    dependencies = {}
    if "not found" in output:
        raise IdentityError("UNRESOLVED_PROVIDER_LIBRARY")
    for line in output.splitlines():
        match = re.fullmatch(r"\s*(\S+)\s+=>\s+(/.*?)\s+\(0x[0-9a-fA-F]+\)\s*", line)
        direct = re.fullmatch(r"\s*(/.*?)\s+\(0x[0-9a-fA-F]+\)\s*", line)
        if match:
            name, path = match.groups()
        elif direct:
            path = direct[1]
            name = Path(path).name
        elif not line.strip() or "linux-vdso" in line:
            continue
        else:
            raise IdentityError("UNRECOGNIZED_LDD_OUTPUT: " + line)
        if name in dependencies:
            raise IdentityError("DUPLICATE_LDD_LIBRARY: " + name)
        dependencies[name] = file_identity(Path(path))
    if not dependencies:
        raise IdentityError("EMPTY_PROVIDER_LINKAGE")
    return dict(sorted(dependencies.items()))


def provider_identity(root, build_dir, env):
    path = build_dir / "examples/di-native-provider"
    override = env.get("SPEC180_NATIVE_PROVIDER_BINARY")
    if override and (root / override).resolve() != path.resolve():
        raise IdentityError("WRONG_PROVIDER_OVERRIDE: " + override)
    artifact = file_identity(path)
    if Path(artifact["realpath"]).parent != path.parent or not os.access(path, os.X_OK):
        raise IdentityError("WRONG_PROVIDER_OUTPUT")
    dependencies = parse_ldd(run(["ldd", str(path)], cwd=root, env=dict(env, LC_ALL="C"),
                                 capture=True, timeout=60))
    for name, library in dependencies.items():
        if name.startswith(LIBRARY):
            check_equal(library["realpath"], str((build_dir / LIBRARY).resolve()),
                        "WRONG_PROVIDER_FRAMEWORK_LIBRARY")
    check_equal(file_identity(path), artifact, "PROVIDER_CHANGED_DURING_PROBE")
    return {"artifact": artifact, "libraries": dependencies}


def read_manifest(path):
    data = json.loads(path.read_text())
    if (not isinstance(data, dict) or data.get("schema") != SCHEMA
            or data.get("scope") != "HOST_LOCAL_ONLY"):
        raise IdentityError("WRONG_MANIFEST_SCOPE")
    for key in ("sources", "configuration", "framework", "runtime", "provider", "ndn_svs",
                "setup_toolchain", "setup_build_flags", "waf_tool"):
        if not isinstance(data.get(key), dict):
            raise IdentityError("INVALID_MANIFEST_FIELD: " + key)
    return data


def atomic_manifest(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + ".", dir=str(path.parent))
    try:
        with os.fdopen(fd, "w") as stream:
            json.dump(data, stream, indent=2, sort_keys=True)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def binding_abi_sources(items):
    """Return source identities that can change the extension's ABI."""
    result = {}
    for key, value in items.items():
        if key.startswith("pythonWrapper/ndnsf/") or key in BINDING_INDEPENDENT_SOURCES:
            continue
        if (key.startswith(BINDING_IMPLEMENTATION_ROOTS)
                and Path(key).suffix in BINDING_IMPLEMENTATION_SUFFIXES):
            continue
        result[key] = value
    return result


def binding_runtime_identity(runtime):
    """Keep loader topology, but ignore hashes of current in-tree DSOs.

    Waf may replace the Core/DI implementation after the extension was built.
    The extension must still load those exact in-tree paths; their current
    hashes are checked by the final manifest/verify path, while requiring a
    Python extension rebuild for an implementation-only change is unnecessary.
    """
    mapped = {}
    for path, identity in runtime["mapped_libraries"].items():
        name = Path(path).name
        if any(name.startswith(prefix) for prefix in BINDING_RUNTIME_LIBRARY_PREFIXES):
            mapped[path] = {key: identity[key] for key in ("path", "realpath")}
        else:
            mapped[path] = identity
    return {"python": runtime["python"], "extension": runtime["extension"],
            "mapped_libraries": mapped}


def binding_reusable(previous, sources, config, runtime, framework, svs, toolchain, flags, waf_tool):
    # setup.py does not track included Core headers. Reuse only when ABI-facing
    # source/configuration and loader topology are unchanged; implementation
    # DSO hashes are intentionally checked after the fresh Waf build instead.
    if not previous:
        return False
    return (binding_abi_sources(previous["sources"]) == binding_abi_sources(sources)
            and previous["configuration"] == config
            and previous["framework"]["realpath"] == framework["realpath"]
            and previous["ndn_svs"] == svs
            and previous["setup_toolchain"] == toolchain
            and previous["setup_build_flags"] == flags
            and previous["waf_tool"] == waf_tool
            and binding_runtime_identity(previous["runtime"]) == binding_runtime_identity(runtime))


def build(root, build_dir, manifest, python, env, jobs=1, binding="auto"):
    sources = source_fingerprints(root)
    config = config_fingerprints(build_dir)
    waf_tool = waf_tool_identity(root, env)
    svs = svs_identity(build_dir)
    flags = setup_build_flags(env)
    previous = None
    if manifest.exists():
        try:
            previous = read_manifest(manifest)
        except (IdentityError, ValueError, OSError):
            pass  # A bad/old receipt can never authorize extension reuse.
    command = [str(root / "waf"), "-o", str(build_dir), "build",
               "-j" + str(jobs), "--targets=" + TARGETS]
    # These optional overrides belong to setup's glue compilation only. Waf
    # continues to use its recorded configuration, including its optimization.
    waf_env = {name: value for name, value in env.items() if name not in SETUP_FLAG_NAMES}
    waf_env["WAFDIR"] = waf_tool["directory"]
    run(command, cwd=root, env=waf_env)
    check_equal(waf_tool_identity(root, env), waf_tool, "WAF_TOOL_CHANGED_DURING_BUILD")
    check_equal(source_fingerprints(root), sources, "SOURCES_CHANGED_DURING_BUILD")
    check_equal(config_fingerprints(build_dir), config, "CONFIG_CHANGED_DURING_BUILD")
    check_equal(svs_identity(build_dir), svs, "SVS_CHANGED_DURING_BUILD")
    toolchain = setup_toolchain_identity(root, env)
    framework = file_identity(build_dir / LIBRARY)
    reusable = False
    if binding == "auto" and previous:
        try:
            runtime = probe_runtime(python, root, env)
            validate_runtime(runtime, root, build_dir)
            validate_svs_mapping(svs, runtime["mapped_libraries"], "WRONG_RUNTIME_NDN_SVS")
            reusable = binding_reusable(previous, sources, config, runtime, framework,
                                        svs, toolchain, flags, waf_tool)
        except (IdentityError, OSError, ValueError, KeyError, subprocess.SubprocessError):
            pass
    commands = [{"argv": command, "cwd": str(root), "WAFDIR": waf_tool["directory"]}]
    if not reusable:
        command = [python, "setup.py", "build_ext", "--inplace", "--force"]
        build_env = dict(env, NDNSF_LIBRARY_DIR=str(build_dir),
                         NDNSF_NDN_SVS_SOURCE_TREE=svs["source_tree"],
                         NDNSF_NDN_SVS_BUILD_TREE=svs["build_tree"])
        build_env.update(toolchain["environment"])
        run(command, cwd=root / "pythonWrapper", env=build_env)
        commands.append({"argv": command, "cwd": str(root / "pythonWrapper"),
                         "NDNSF_LIBRARY_DIR": str(build_dir),
                         **toolchain["environment"],
                         **flags,
                         SVS_SOURCE_ENV: svs["source_tree"], SVS_BUILD_ENV: svs["build_tree"]})
    runtime = probe_runtime(python, root, env)
    validate_runtime(runtime, root, build_dir)
    validate_svs_mapping(svs, runtime["mapped_libraries"], "WRONG_RUNTIME_NDN_SVS")
    provider = provider_identity(root, build_dir, env)
    validate_svs_mapping(svs, provider["libraries"], "WRONG_PROVIDER_NDN_SVS")
    check_equal(source_fingerprints(root), sources, "SOURCES_CHANGED_DURING_BUILD")
    check_equal(config_fingerprints(build_dir), config, "CONFIG_CHANGED_DURING_BUILD")
    check_equal(svs_identity(build_dir), svs, "SVS_CHANGED_DURING_BUILD")
    check_equal(setup_toolchain_identity(root, env), toolchain, "SETUP_TOOLCHAIN_CHANGED_DURING_BUILD")
    check_equal(waf_tool_identity(root, env), waf_tool, "WAF_TOOL_CHANGED_DURING_BUILD")
    check_equal(file_identity(build_dir / LIBRARY), framework, "FRAMEWORK_CHANGED_DURING_BUILD")
    data = {"schema": SCHEMA, "scope": "HOST_LOCAL_ONLY", "root": str(root),
            "build_dir": str(build_dir), "sources": sources, "configuration": config,
            "framework": framework, "runtime": runtime, "provider": provider, "ndn_svs": svs,
            "setup_toolchain": toolchain,
            "setup_build_flags": flags,
            "waf_tool": waf_tool,
            "commands": commands, "binding_reused": reusable}
    atomic_manifest(manifest, data)
    return data


def verify(root, build_dir, manifest, python, env):
    data = read_manifest(manifest)
    check_equal([data["root"], data["build_dir"]], [str(root), str(build_dir)],
                "WRONG_BUILD_ROOT")
    check_equal(source_fingerprints(root), data["sources"], "STALE_SOURCES")
    check_equal(config_fingerprints(build_dir), data["configuration"], "STALE_CONFIGURATION")
    check_equal(waf_tool_identity(root, env), data["waf_tool"], "WAF_TOOL_CHANGED")
    svs = svs_identity(build_dir)
    check_equal(svs, data["ndn_svs"], "STALE_NDN_SVS_INPUTS")
    check_equal(setup_toolchain_identity(root, env), data["setup_toolchain"], "STALE_SETUP_TOOLCHAIN")
    check_equal(file_identity(build_dir / LIBRARY), data["framework"], "FRAMEWORK_CHANGED")
    check_equal(file_identity(build_dir / "examples/di-native-provider"),
                data["provider"]["artifact"], "PROVIDER_CHANGED")
    runtime = probe_runtime(python, root, env)
    validate_runtime(runtime, root, build_dir)
    validate_svs_mapping(svs, runtime["mapped_libraries"], "WRONG_RUNTIME_NDN_SVS")
    check_equal(runtime, data["runtime"], "RUNTIME_IDENTITY_CHANGED")
    provider = provider_identity(root, build_dir, env)
    validate_svs_mapping(svs, provider["libraries"], "WRONG_PROVIDER_NDN_SVS")
    check_equal(provider, data["provider"], "PROVIDER_LINKAGE_CHANGED")
    check_equal(source_fingerprints(root), data["sources"], "SOURCES_CHANGED_DURING_VERIFY")
    check_equal(config_fingerprints(build_dir), data["configuration"],
                "CONFIG_CHANGED_DURING_VERIFY")
    check_equal(waf_tool_identity(root, env), data["waf_tool"], "WAF_TOOL_CHANGED_DURING_VERIFY")
    check_equal(svs_identity(build_dir), svs, "SVS_CHANGED_DURING_VERIFY")
    check_equal(file_identity(build_dir / LIBRARY), data["framework"], "FRAMEWORK_CHANGED")
    check_equal(file_identity(data["runtime"]["extension"]["path"]),
                data["runtime"]["extension"], "EXTENSION_CHANGED")
    check_equal(file_identity(build_dir / "examples/di-native-provider"),
                data["provider"]["artifact"], "PROVIDER_CHANGED")
    return data


def verify_local_runtime(project_root, manifest_path=None, *,
                         python_executable=None, environ=None):
    """Runner API: read-only, before MiniNDN side effects; raise IdentityError.

    Selection: explicit manifest_path > SPEC180_NATIVE_BUILD_MANIFEST >
    ROOT/build-system-j2/spec180-native-build.json. Relative overrides are
    relative to project_root. Pass the *actual later child environment* here;
    no import or loader path is added. The caller selects this API only for a
    host checkout (ROOT/.git exists); containers use their sealed closure.
    Success returns the verified manifest dict, never a qualification verdict.
    """
    try:
        root = Path(project_root).resolve(strict=True)
        env = dict(os.environ if environ is None else environ)
        chosen = manifest_path or env.get(MANIFEST_ENV) or DEFAULT_MANIFEST
        manifest = (root / chosen).absolute()
        if not manifest.is_file():
            raise IdentityError("LOCAL_MANIFEST_MISSING: " + str(manifest))
        data = read_manifest(manifest)
        build_dir = Path(data["build_dir"]).resolve(strict=True)
        if root not in build_dir.parents:
            raise IdentityError("BUILD_DIR_MUST_BE_WITHIN_REPOSITORY")
        python = python_executable or sys.executable
        return verify(root, build_dir, manifest, python, env)
    except IdentityError:
        raise
    except (OSError, ValueError, KeyError, TypeError,
            subprocess.SubprocessError) as error:
        raise IdentityError("LOCAL_IDENTITY_INVALID: " + str(error)) from error


@contextmanager
def local_lock(build_dir):
    # All invocations share this lock; unrelated direct Waf invocations must be
    # coordinated by the caller. Snapshot comparisons additionally catch drift.
    with (build_dir / ".spec180-native-build.lock").open("a") as lock:
        try:
            fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise IdentityError("LOCAL_BUILD_BUSY") from error
        yield


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", choices=("build", "verify"))
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--build-dir", type=Path, default=Path("build-system-j2"))
    parser.add_argument("--manifest", type=Path,
                        help="Override SPEC180_NATIVE_BUILD_MANIFEST; default: BUILD_DIR/spec180-native-build.json")
    parser.add_argument("--python", default=sys.executable, help="Python used by the later local tests")
    parser.add_argument("--jobs", type=int, default=1)
    parser.add_argument("--binding", choices=("auto", "always"), default="auto")
    args = parser.parse_args(argv)
    try:
        if not sys.platform.startswith("linux") or any(os.environ.get(name) for name in
                ("APPTAINER_CONTAINER", "SINGULARITY_CONTAINER")):
            raise IdentityError("HOST_LOCAL_ONLY")
        if args.jobs < 1:
            raise IdentityError("JOBS_MUST_BE_POSITIVE")
        root = args.root.resolve(strict=True)
        build_dir = (root / args.build_dir).resolve(strict=True)
        if build_dir == root or root not in build_dir.parents:
            raise IdentityError("BUILD_DIR_MUST_BE_WITHIN_REPOSITORY")
        chosen = args.manifest or os.environ.get(MANIFEST_ENV)
        manifest = (root / chosen).absolute() if chosen else build_dir / "spec180-native-build.json"
        # Preserve the selected venv's symlink path when launching Python.
        python = shutil.which(args.python)
        if not python:
            raise IdentityError("PYTHON_NOT_FOUND")
        env = dict(os.environ)
        if args.command == "build":
            with local_lock(build_dir):
                build(root, build_dir, manifest, python, env, args.jobs, args.binding)
        else:
            verify(root, build_dir, manifest, python, env)
        print("SPEC180_NATIVE_IDENTITY_OK " + str(manifest))
        return 0
    except (IdentityError, OSError, ValueError, KeyError, TypeError,
            subprocess.SubprocessError) as error:
        print("SPEC180_NATIVE_IDENTITY_REJECTED: " + str(error), file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
