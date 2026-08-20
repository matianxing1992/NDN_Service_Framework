import os
import runpy
import shutil
import subprocess
import sys
from pathlib import Path

import pytest


ROOT = Path(__file__).resolve().parents[2]
WAF_RUNTIME = next(ROOT.glob(".waf3-*"))
sys.path.insert(0, str(WAF_RUNTIME))


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
