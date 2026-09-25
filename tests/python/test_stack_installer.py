"""Installer control-flow checks; never run apt, builds, or system installation."""
import os
from pathlib import Path
import subprocess

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "install_ndnsf_stack.sh"
DEFINITIONS, MAIN = SCRIPT.read_text().split('\ncd "$ROOT"\n', 1)


def shell(code, *args, env=None):
    return subprocess.run(
        ["bash", "-c", code, str(SCRIPT), *args], text=True,
        capture_output=True, env=env, timeout=10,
    )


@pytest.mark.parametrize("args, message", [
    (["--no-configure"], "--no-configure is not allowed"),
    (["--python"], "--python requires"),
    (["--deps-dir"], "--deps-dir requires"),
    (["--jobs"], "--jobs requires"),
    (["--jobs", "0"], "positive integer"),
    (["--jobs", "2; touch /tmp/not-executed"], "positive integer"),
])
def test_invalid_options_stop_before_installation(args, message):
    result = shell(SCRIPT.read_text(), *args)
    assert result.returncode == 2
    assert message in result.stderr
    assert "NDNSF stack install root" not in result.stdout


def test_jobs_environment_and_override():
    env = dict(os.environ, NDNSF_BUILD_JOBS="2")
    assert shell(DEFINITIONS + '\nprintf "%s" "$JOBS"', env=env).stdout == "2"
    assert shell(DEFINITIONS + '\nprintf "%s" "$JOBS"', "--jobs", "3", env=env).stdout == "3"


def test_legacy_unpinned_worktree_is_not_reused(tmp_path):
    source = tmp_path / "with space and ' quote" / "ndn-svs"
    source.mkdir(parents=True)
    (source / ".git").write_text("gitdir: /not-needed-for-this-check\n")
    result = shell(DEFINITIONS + '\nensure_source_tree ndn-svs invalid-url',
                   "--deps-dir", str(source.parent))
    assert result.returncode != 0
    assert "URL differs from lock" in result.stderr
    assert (source / '.git').read_text().startswith('gitdir:')


def test_missing_sdk_stops_before_system_changes():
    result = shell(DEFINITIONS + '''
require_global_sdk_pkg() { echo missing-sdk >&2; exit 31; }
install_common_system_packages() { echo UNEXPECTED_APT; }
build_waf_dependency() { echo UNEXPECTED_BUILD; }
install_external_dependencies
''')
    assert result.returncode == 31
    assert "UNEXPECTED" not in result.stdout


def test_waf_dependency_handles_quoted_source_path(tmp_path):
    source = tmp_path / "source ' with spaces" / "ndn-svs"
    source.mkdir(parents=True)
    (source / ".git").write_text("gitdir: unused\n")
    waf = source / "waf"
    waf.write_text('#!/bin/bash\nprintf "FAKE_WAF:%s:%s\\n" "$PWD" "$*"\n')
    waf.chmod(0o755)
    result = shell(DEFINITIONS + '''
FORCE_DEPENDENCIES=1
sudo_run() { :; }
ensure_source_tree() { printf '%s\n' "$DEPS_DIR/ndn-svs"; }
record_source_receipt() { :; }
build_waf_dependency ndn-svs libndn-svs invalid-url 0.1
''', "--deps-dir", str(source.parent), "--jobs", "2")
    assert result.returncode == 0, result.stderr
    assert f"FAKE_WAF:{source}:configure --prefix=/usr/local" in result.stdout
    assert f"FAKE_WAF:{source}:-j2" in result.stdout


def test_install_refreshes_loader_before_python_and_limits_jobs():
    stubs = '''
require_system_toolchain() { :; }
require_global_external_closure() { :; }
require_source_receipts() { :; }
source_field() { printf '%s\n' 'https://example.invalid/test.git'; }
require_global_file() { :; }
run_waf_clean() { echo "WAF:$*"; }
sudo_run() { echo "SUDO:$*"; }
native_digest_receipt() { echo '{}'; }
pip_install() { echo "PIP:$*"; }
run() { :; }
'''
    result = shell(DEFINITIONS + stubs + MAIN, "--no-dependencies", "--jobs", "2")
    assert result.returncode == 0, result.stderr
    assert "WAF:-j2" in result.stdout
    assert "./waf install -j2" in result.stdout
    assert result.stdout.index("SUDO:/sbin/ldconfig") < result.stdout.index("PIP:")


def test_build_only_does_not_install_python():
    stubs = '''
require_system_toolchain() { :; }
require_global_external_closure() { :; }
require_source_receipts() { :; }
source_field() { printf '%s\n' 'https://example.invalid/test.git'; }
run_waf_clean() { :; }
pip_install() { echo UNEXPECTED_PIP; }
sudo_run() { echo UNEXPECTED_SUDO; }
'''
    result = shell(DEFINITIONS + stubs + MAIN, "--no-dependencies", "--no-system-install")
    assert result.returncode == 2
    assert "UNEXPECTED" not in result.stdout


def test_plan_is_offline_and_uses_experimental_pins(tmp_path):
    result = subprocess.run(['bash', str(SCRIPT), '--source', '--plan',
                             '--deps-dir', str(tmp_path / 'absent'),
                             '--', '--with-tests'], capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    assert 'ref=Experimental' in result.stdout
    assert '9f2d8a47cd2a25a5f9ade661c9dbe8acd6416a20' in result.stdout
    assert '--with-tests' in result.stdout
    assert 'Preinstalled SDKs:' in result.stdout
    assert not (tmp_path / 'absent').exists()


def test_configure_modes_conflict_before_side_effects():
    for args in [('--configure-only', '--source'), ('--no-install', '--deps-only'),
                 ('--check-dependencies', '--source')]:
        result = shell(SCRIPT.read_text(), *args)
        assert result.returncode == 2
        assert 'cannot be combined' in result.stderr


def test_deps_only_stops_before_waf_and_python():
    stubs = '''
require_system_toolchain() { :; }
source_field() { echo https://example.invalid/repo.git; }
install_external_dependencies() { echo DEPS_ONLY_OK; }
run_waf_clean() { echo UNEXPECTED_WAF; }
pip_install() { echo UNEXPECTED_PIP; }
'''
    result = shell(DEFINITIONS + stubs + MAIN, '--source', '--deps-only')
    assert result.returncode == 0, result.stderr
    assert 'DEPS_ONLY_OK' in result.stdout
    assert 'UNEXPECTED' not in result.stdout


def test_version_match_without_receipt_does_not_skip():
    result = shell(DEFINITIONS + '''
is_pkg_installed() { return 0; }
has_source_receipt() { return 1; }
ensure_source_tree() { echo SOURCE_REQUIRED >&2; return 23; }
build_waf_dependency ndn-svs libndn-svs unused 0.1
''')
    assert result.returncode == 23
    assert 'SOURCE_REQUIRED' in result.stderr


def test_version_and_receipt_match_skips_build():
    result = shell(DEFINITIONS + '''
is_pkg_installed() { return 0; }
has_source_receipt() { return 0; }
ensure_source_tree() { echo UNEXPECTED; return 23; }
build_waf_dependency ndn-svs libndn-svs unused 0.1
''')
    assert result.returncode == 0
    assert 'skipping' in result.stdout
    assert 'UNEXPECTED' not in result.stdout
