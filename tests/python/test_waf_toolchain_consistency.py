import os
import hashlib
import json
import runpy
import shutil
import subprocess
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest


ROOT = Path(__file__).resolve().parents[2]
WAF_RUNTIME = next(ROOT.glob(".waf3-*"))
sys.path.insert(0, str(WAF_RUNTIME))


@pytest.mark.parametrize("lld_compatible", [True, False])
def test_lld_is_selected_only_when_project_link_flags_are_supported(
    monkeypatch, lld_compatible
):
    module = runpy.run_path(str(ROOT / ".waf-tools/default-compiler-flags.py"))
    monkeypatch.setattr(module["Utils"], "unversioned_sys_platform", lambda: "linux")
    calls = []

    def check_cxx(**kwargs):
        calls.append(kwargs)
        return lld_compatible

    conf = SimpleNamespace(check_cxx=check_cxx)
    flags = module["GccBasicFlags"]().getGeneralFlags(conf)

    assert flags["LINKFLAGS"] == (["-fuse-ld=lld"] if lld_compatible else [])
    assert calls == [{
        "linkflags": ["-fuse-ld=lld", "-Wl,--no-relax"], "mandatory": False
    }]


def test_configured_compiler_resolves_matching_binutils_when_path_is_polluted(tmp_path):
    marker = tmp_path / "fake-ld-invoked"
    fake_ld = tmp_path / "ld"
    fake_ld.write_text(
        f"#!/bin/sh\nprintf invoked > '{marker}'\nexit 99\n",
        encoding="utf-8",
    )
    fake_ld.chmod(0o755)

    wscript = runpy.run_path(str(ROOT / "wscript"))
    resolve = wscript["_resolve_compiler_toolchain"]
    compiler = shutil.which("g++", path="/usr/bin:/bin")
    assert compiler is not None
    compiler_dir = str(Path(compiler).resolve().parent)
    env = os.environ.copy()
    env["PATH"] = f"{tmp_path}:/home/linuxbrew/.linuxbrew/bin:/usr/bin:/bin"

    tools = resolve(compiler, env=env)

    assert tools["compiler_dir"] == compiler_dir
    assert tools["search_flag"] == f"-B{compiler_dir}"
    for tool in ("ld", "ar", "ranlib", "nm"):
        assert os.path.commonpath([compiler_dir, tools[tool]]) == compiler_dir
    assert all(str(tmp_path) not in value for value in tools.values())

    source = tmp_path / "probe.cpp"
    executable = tmp_path / "probe"
    source.write_text("int main() { return 0; }\n", encoding="utf-8")
    completed = subprocess.run(
        [compiler, tools["search_flag"], str(source), "-o", str(executable)],
        env=env,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr
    assert executable.is_file()
    assert not marker.exists(), "the PATH-injected linker executed"


def test_configured_compiler_outside_required_root_is_rejected(tmp_path):
    fake_compiler = tmp_path / "g++"
    fake_compiler.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
    fake_compiler.chmod(0o755)

    resolve = runpy.run_path(str(ROOT / "wscript"))["_resolve_compiler_toolchain"]

    with pytest.raises(RuntimeError, match="outside the required toolchain root"):
        resolve(str(fake_compiler), env=os.environ.copy(), expected_root="/usr/bin")


@pytest.mark.parametrize("case, failure", [
    ("selected-sdk", None),
    ("default-layout", None),
    ("wrong-sdk", "identity path is outside the canonical root"),
    ("escaping-symlink", "realpath is outside the canonical root"),
    ("digest", "digest changed"),
    ("soname", "SONAME changed"),
    ("other-library", "identity path is outside the canonical root"),
])
def test_global_identity_binds_only_nac_to_selected_sdk(tmp_path, monkeypatch, case, failure):
    validate = runpy.run_path(str(ROOT / "wscript"))["_validate_global_dependency_identity"]
    namespace = validate.__globals__
    global_root = tmp_path / "global"
    selected = global_root if case == "default-layout" else global_root / "sdk" / "selected"
    name = "libndn-cxx.so" if case == "other-library" else "libnac-abe.so"
    library = selected / "lib" / name
    if case == "wrong-sdk":
        library = global_root / "sdk" / "another" / "lib" / name
    library.parent.mkdir(parents=True)
    # The receipt verifier checks ELF identity, not NAC ABI. A small existing
    # system ELF has no SONAME; the actual Waf configure checks the real SDK.
    shutil.copyfile("/bin/true", library)
    if case == "escaping-symlink":
        outside = global_root / "outside" / name
        outside.parent.mkdir()
        shutil.copyfile(library, outside)
        library.unlink()
        library.symlink_to(outside)
    receipt = tmp_path / "identity.json"
    receipt.write_text(json.dumps({"schema": 1, "libraries": {name: {
        "path": str(library), "realpath": str(library.resolve()),
        "sha256": "0" * 64 if case == "digest" else hashlib.sha256(library.read_bytes()).hexdigest(),
        "soname": "unexpected-soname" if case == "soname" else "",
    }}}))
    monkeypatch.delenv("NDNSF_CONTAINER_BUILD", raising=False)
    monkeypatch.setitem(namespace, "HOST_DEPENDENCY_IDENTITY_FILE", str(receipt))
    monkeypatch.setitem(namespace, "HOST_DEPENDENCY_LIBRARIES", (name,))
    monkeypatch.setitem(namespace, "_global_dependency_prefixes", lambda: (str(global_root),))

    def fatal(message):
        raise RuntimeError(message)

    conf = SimpleNamespace(options=SimpleNamespace(nac_abe_prefix=str(selected)), fatal=fatal)
    if failure is None:
        validate(conf)
    else:
        with pytest.raises(RuntimeError, match=failure):
            validate(conf)
