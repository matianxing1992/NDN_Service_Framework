"""Installer control-flow checks; never run apt, builds, or system installation."""
import hashlib
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile

import pytest


ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "install_ndnsf_stack.sh"
DEFINITIONS, MAIN = SCRIPT.read_text().split('\ncd "$ROOT"\n', 1)


def shell(code, *args, env=None):
    # Execute a file, as the real installer does. bash -c both loses the
    # BASH_SOURCE file identity and hits Linux's per-argument size limit for
    # the full installer. Only its repository-root setup is fixture-specific.
    code = code.replace(DEFINITIONS, DEFINITIONS +
                        '\nrun_bounded() { run "$@"; }\n', 1)
    root_assignment = 'ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"'
    code = code.replace(root_assignment, 'ROOT=' + shlex.quote(str(ROOT)), 1)
    with tempfile.TemporaryDirectory(prefix='ndnsf-installer-test-') as directory:
        fixture = Path(directory) / 'installer.sh'
        fixture.write_text(code)
        # Fixtures must never share a live installer's default SDK scratch/lock.
        fixture_env = dict(os.environ if env is None else env)
        fixture_env.setdefault("NDNSF_BUILD_DIR", str(Path(directory) / "sdk-build"))
        return subprocess.run(
            ["bash", str(fixture), *args], text=True,
            capture_output=True, env=fixture_env, timeout=10,
        )


def test_yolo_launcher_preflights_its_prefix_outside_checkout(tmp_path):
    prefix = tmp_path / "prefix"
    bin_dir = prefix / "bin"
    bin_dir.mkdir(parents=True)
    launcher = bin_dir / "ndnsf-yolo-minindn"
    launcher_body = DEFINITIONS.split("ndnsf-yolo-minindn\" <<'LAUNCHER'\n", 1)[1]
    launcher.write_text(launcher_body.split("\nLAUNCHER\n", 1)[0] + "\n")
    launcher.chmod(0o755)

    state = prefix / "share/ndnsf/yolo-minindn"
    experiments = state / "experiments"
    experiments.mkdir(parents=True)
    for name in ("NDNSF_DI_YoloSplit_Minindn.py", "NDNSF_NewAPI_Minindn_Perf.py"):
        shutil.copyfile(ROOT / "Experiments" / name, experiments / name)
    actors = state / "actors"
    shutil.copytree(ROOT / "examples/python/NDNSF-DistributedInference/yolo_split", actors)
    native_lib = prefix / "lib"
    native_lib.mkdir()
    (native_lib / "libndnsf-distributed-inference.so").touch()
    (state / "native-library-dir").write_text(str(native_lib) + "\n")
    (state / "python-executable").write_text(sys.executable + "\n")
    sites = [prefix / name for name in ("mini-site", "yolo-site")]
    for site in sites:
        site.mkdir()
    (state / "python-site-paths").write_text("".join(f"{site}\n" for site in sites))
    model = state.parent / "models/yolo26n.pt"
    model.parent.mkdir()
    model.write_bytes(b"test model")
    (state / "yolo26n.sha256").write_text(hashlib.sha256(model.read_bytes()).hexdigest() + "\n")
    env = dict(os.environ, NDNSF_ROOT=str(tmp_path / "absent-checkout"), TMPDIR=str(tmp_path))
    command = [str(launcher), "--preflight-only"]

    preflight = subprocess.run(command, cwd=tmp_path, env=env, capture_output=True, text=True)
    assert preflight.returncode == 0, preflight.stderr
    assert "YOLO_MININDN_INSTALLED_INPUTS_OK" in preflight.stdout
    model.write_bytes(b"corrupted model")
    corrupt = subprocess.run(command, cwd=tmp_path, env=env, capture_output=True, text=True)
    assert corrupt.returncode != 0
    assert "failed SHA-256" in corrupt.stderr
    (actors / "yolo_split_lib.py").unlink()
    missing = subprocess.run(command, cwd=tmp_path, env=env, capture_output=True, text=True)
    assert missing.returncode != 0
    assert "actors/yolo_split_lib.py" in missing.stderr
    assert not list(tmp_path.glob("ndnsf-yolo-run.*"))


@pytest.mark.parametrize("args, message", [
    (["--no-configure"], "--no-configure is not allowed"),
    (["--python"], "--python requires"),
    (["--deps-dir"], "--deps-dir requires"),
    (["--jobs"], "--jobs requires"),
    (["--jobs", "0"], "host build"),
    (["--jobs", "2; touch /tmp/not-executed"], "host build"),
    (["--", "--out=/tmp/other"], "ordinary checkout build/"),
    (["--", "--sanitize-di"], "ordinary checkout build/"),
])
def test_invalid_options_stop_before_installation(args, message):
    result = shell(SCRIPT.read_text(), '--plan', *args)
    assert result.returncode == 2
    assert message in result.stderr
    assert "NDNSF stack install root" not in result.stdout


def test_prepare_experiment_delegates_only_to_selected_installed_tool(tmp_path):
    import json
    prefix = tmp_path / 'prefix'
    helper = prefix / 'share/ndnsf/Experiments/prepare-native-deployment.py'
    helper.parent.mkdir(parents=True)
    helper.write_text('import json,sys; print(json.dumps(sys.argv[1:]))\n')
    work = tmp_path / 'new private inputs'
    manifest = tmp_path / 'asset-manifest.json'
    result = shell(SCRIPT.read_text(), '--prepare-experiment', 'qwen06b-tp-cpu',
                   '--prefix', str(prefix), '--asset-manifest', str(manifest),
                   '--work-root', str(work), '--namespace', '/test/cpu', '--offline')
    assert result.returncode == 0, result.stderr
    assert json.loads(result.stdout) == ['--profile', 'qwen06b-tp-cpu', '--prefix', str(prefix),
        '--asset-manifest', str(manifest), '--work-root', str(work), '--namespace', '/test/cpu', '--offline']
    assert not work.exists() and not manifest.exists()


def test_manifest_python_console_paths_collapse_record_parents(tmp_path):
    import json
    source = SCRIPT.read_text().split("<<'NDNSF_INSTALL_MANIFEST_PY'\n", 1)[1].split('\nNDNSF_INSTALL_MANIFEST_PY', 1)[0]
    namespace = {'__name__': 'installer_manifest_test'}
    exec(compile(source, str(SCRIPT), 'exec'), namespace)
    site = tmp_path / 'venv/lib/python3.8/site-packages'
    site.mkdir(parents=True)
    console = tmp_path / 'venv/bin/ndnsf-di'
    alias = site / '../../../bin/ndnsf-di'
    assert namespace['canonical_manifest_path'](alias) == console
    state = {'installed': {'kind': 'file', 'sha256': 'a' * 64}, 'owners': ['python:ops']}
    manifest = tmp_path / 'manifest.json'
    manifest.write_text(json.dumps(dict(schema=1, sources=[], files={str(alias): state})))
    assert namespace['read_manifest'](manifest)['files'] == {str(console): state}
    conflicting = dict(state, installed={'kind': 'file', 'sha256': 'b' * 64})
    manifest.write_text(json.dumps(dict(schema=1, sources=[], files={str(alias): state, str(console): conflicting})))
    with pytest.raises(ValueError, match='conflicting manifest aliases'):
        namespace['read_manifest'](manifest)


def test_prepare_experiment_plan_does_not_need_or_create_an_installation(tmp_path):
    prefix = tmp_path / 'uninstalled'
    result = shell(SCRIPT.read_text(), '--plan', '--prepare-experiment', 'qwen06b-tp-cpu',
        '--prefix', str(prefix), '--asset-manifest', str(tmp_path / 'assets.json'),
        '--work-root', str(tmp_path / 'work'), '--namespace', '/test/cpu', '--offline')
    assert result.returncode == 0, result.stderr
    assert 'Installed experiment preparation (no writes)' in result.stdout
    assert '--offline' in result.stdout and str(prefix) in result.stdout
    assert not prefix.exists() and not (tmp_path / 'work').exists()


@pytest.mark.parametrize('args', [
    ['--prepare-experiment', 'qwen06b-tp-cpu'],
    ['--asset-manifest', '/missing/assets.json'],
    ['--offline'],
])
def test_incomplete_experiment_arguments_fail_before_installation(args):
    result = shell(SCRIPT.read_text(), *args)
    assert result.returncode == 2 and '--prepare-experiment' in result.stderr
    assert 'NDNSF stack install root' not in result.stdout


def test_jobs_environment_and_override():
    env = dict(os.environ, NDNSF_BUILD_JOBS="2")
    assert shell(DEFINITIONS + '\nprintf "%s" "$JOBS"', env=env).stdout == "2"
    assert shell(DEFINITIONS + '\nprintf "%s" "$JOBS"', "--jobs", "1", env=env).stdout == "1"


def test_person_runtime_plan_does_not_create_environment(tmp_path):
    destination = tmp_path / "person-cpu"
    result = shell(SCRIPT.read_text(), "--person-runtime", str(destination), "--plan")
    assert result.returncode == 0, result.stderr
    assert "Person CPU runtime:" in result.stdout
    assert not destination.exists()


def test_person_runtime_requires_verified_wheel_before_creating_environment(tmp_path):
    python = shutil.which("python3.10")
    if python is None:
        pytest.skip("CPython 3.10 is not installed")
    destination = tmp_path / "person-cpu"
    wheels = tmp_path / "wheels"
    wheels.mkdir()
    (wheels / "torchreid-1.4.0-py3-none-any.whl").write_bytes(b"untrusted wheel")
    result = subprocess.run(
        ["bash", str(ROOT / "scripts/install-person-runtime.sh"), str(destination)],
        env=dict(os.environ, NDNSF_PERSON_PYTHON=python, NDNSF_PERSON_WHEELS=str(wheels)),
        text=True, capture_output=True, timeout=10,
    )
    assert result.returncode != 0
    assert "Missing hash-verified Torchreid" in result.stderr
    assert not destination.exists()


def test_installer_defaults_to_host_memory_policy():
    environment = dict(os.environ)
    environment.pop("NDNSF_BUILD_JOBS", None)
    result = shell(DEFINITIONS + '\nprintf "%s" "$JOBS"', env=environment)
    assert result.returncode == 0
    total_bytes = int(next(line.split()[1] for line in Path('/proc/meminfo').read_text().splitlines()
                           if line.startswith('MemTotal:'))) * 1024
    expected = subprocess.check_output(['nproc'], text=True).strip() if total_bytes > 100_000_000_000 else '6'
    assert result.stdout == expected


def test_person_runtime_rejects_disposable_parent_environment(tmp_path):
    python = shutil.which("python3.10")
    if python is None:
        pytest.skip("CPython 3.10 is not installed")
    parent = tmp_path / "parent"
    subprocess.run([python, "-m", "venv", "--without-pip", str(parent)], check=True)
    destination = tmp_path / "person-cpu"
    result = subprocess.run(
        ["bash", str(ROOT / "scripts/install-person-runtime.sh"), str(destination)],
        env=dict(os.environ, NDNSF_PERSON_PYTHON=str(parent / "bin/python")),
        text=True, capture_output=True, timeout=10,
    )
    assert result.returncode != 0
    assert "stable base interpreter" in result.stderr
    assert not destination.exists()


@pytest.mark.parametrize("version", ["1.71.0", "1.85.0", "1.90.0"])
def test_minindn_reuses_matching_installed_boost_without_source_pin(tmp_path, version):
    library = tmp_path / "boost"
    library.mkdir()
    soname = "libboost_iostreams.so." + version
    (library / soname).write_bytes(b"fixture")
    (library / "libboost_iostreams.so").symlink_to(soname)
    result = shell(DEFINITIONS + '''
BOOST_VERSION_STRING="$TEST_BOOST_VERSION"
BOOST_LIBRARY_DIR="$TEST_BOOST_LIBRARY"
GLOBAL_LIBRARY_DIR="$TEST_BOOST_LIBRARY"
readelf() { printf ' Library soname: [libboost_iostreams.so.%s]\\n' "$BOOST_VERSION_STRING"; }
run() { echo UNEXPECTED_SOURCE_BUILD >&2; return 99; }
ensure_minindn_boost_iostreams
''', env=dict(os.environ, TEST_BOOST_VERSION=version, TEST_BOOST_LIBRARY=str(library)))
    assert result.returncode == 0, result.stderr
    assert "Reusing Boost " + version in result.stdout
    assert "UNEXPECTED_SOURCE_BUILD" not in result.stderr


def test_minindn_missing_unpinned_boost_refuses_source_download(tmp_path):
    result = shell(DEFINITIONS + '''
BOOST_VERSION_STRING=1.90.0
BOOST_LIBRARY_DIR="$TEST_BOOST_LIBRARY"
GLOBAL_LIBRARY_DIR="$TEST_BOOST_LIBRARY"
run() { echo UNEXPECTED_SOURCE_BUILD >&2; return 99; }
ensure_minindn_boost_iostreams
''', env=dict(os.environ, TEST_BOOST_LIBRARY=str(tmp_path)))
    assert result.returncode != 0
    assert "No pinned Boost 1.90.0 archive" in result.stderr
    assert "UNEXPECTED_SOURCE_BUILD" not in result.stderr


def test_minindn_installed_boost_with_wrong_soname_is_rejected(tmp_path):
    soname = "libboost_iostreams.so.1.90.0"
    (tmp_path / soname).write_bytes(b"fixture")
    (tmp_path / "libboost_iostreams.so").symlink_to(soname)
    result = shell(DEFINITIONS + '''
BOOST_VERSION_STRING=1.90.0
BOOST_LIBRARY_DIR="$TEST_BOOST_LIBRARY"
GLOBAL_LIBRARY_DIR="$TEST_BOOST_LIBRARY"
readelf() { echo ' Library soname: [libboost_iostreams.so.1.85.0]'; }
ensure_minindn_boost_iostreams
''', env=dict(os.environ, TEST_BOOST_LIBRARY=str(tmp_path)))
    assert result.returncode != 0
    assert "Reusing Boost" not in result.stdout


def test_native_prefix_drives_waf_and_receipt(tmp_path):
    prefix = tmp_path / "runtime"
    library = prefix / "lib"
    library.mkdir(parents=True)
    names = ("libndn-service-framework.so", "libndnsf-distributed-inference.so",
             "libndnsf-distributed-repo.a")
    for name in names:
        (library / name).write_bytes(name.encode())
    result = shell(DEFINITIONS + '''
refresh_global_pkg_config_path() { GLOBAL_PKG_CONFIG_PATH=/usr/local/lib/pkgconfig; }
run_bounded() { printf '%s\\n' "$@"; }
run_waf_clean configure
native_digest_receipt
printf 'LEDGER=%s\\n' "$INSTALL_MANIFEST"
''', "--prefix", str(prefix))
    assert result.returncode == 0, result.stderr
    assert "--prefix=" + str(prefix) in result.stdout.splitlines()
    assert "--libdir=" + str(library) in result.stdout.splitlines()
    assert "NDNSF_LIBRARY_DIR=" + str(library) in result.stdout.splitlines()
    assert ("NDNSF_RUNTIME_RPATH=" + str(library) + ":/usr/local/lib"
            in result.stdout.splitlines())
    import hashlib
    import json
    receipt = next(json.loads(line) for line in result.stdout.splitlines() if line.startswith('{'))
    assert receipt == {name: hashlib.sha256(name.encode()).hexdigest() for name in names}
    assert "LEDGER=" + str(prefix / "share/ndnsf/stack-install-manifest.json") in result.stdout


def test_installed_runtime_rpath_orders_release_before_sdk_and_deduplicates():
    result = shell(DEFINITIONS + '''
NAC_SDK_PREFIX=/usr/local/ndnsf-sdk/nac-abe/fixture
native_runtime_rpath
''', '--prefix', '/opt/ndnsf-fixture')
    assert result.returncode == 0, result.stderr
    assert result.stdout == ('/opt/ndnsf-fixture/lib:'
                             '/usr/local/ndnsf-sdk/nac-abe/fixture/lib:/usr/local/lib')
    default = shell(DEFINITIONS + '''
NAC_SDK_PREFIX=/usr/local
native_runtime_rpath
''')
    assert default.returncode == 0, default.stderr
    assert default.stdout == '/usr/local/lib'


@pytest.mark.parametrize("profile,di,repo,uav", [
    ("all", 1, 1, 1), ("core", 0, 0, 0), ("repo", 0, 1, 0), ("di", 1, 1, 0),
])
def test_components_drive_installer_and_waf(profile, di, repo, uav):
    result = shell(DEFINITIONS + '''
printf 'SELECTED=%s,%s,%s\\n' "$NDNSF_INSTALL_DI" "$NDNSF_INSTALL_REPO" "$NDNSF_INSTALL_UAV"
refresh_global_pkg_config_path() { :; }
run_bounded() { printf '%s\\n' "$@"; }
run_waf_clean configure
''', "--components", profile)
    assert result.returncode == 0, result.stderr
    assert f"SELECTED={di},{repo},{uav}" in result.stdout
    assert "--components=" + profile in result.stdout.splitlines()


@pytest.mark.parametrize("args", [
    ["--components", "unknown"],
    ["--components", "di", "--with-examples"],
    ["--components", "repo", "--with-qwen-minindn"],
    ["--", "--components=core"],
])
def test_invalid_component_combination_stops_before_mutation(args):
    result = shell(SCRIPT.read_text(), "--plan", *args)
    assert result.returncode == 2
    assert not result.stdout


@pytest.mark.parametrize("profile", ["core", "repo", "di"])
def test_component_native_tests_are_supported_without_examples(profile):
    result = shell(SCRIPT.read_text(), "--plan", "--components", profile, "--with-tests")
    assert result.returncode == 0, result.stderr
    assert "Waf configure options: --with-tests" in result.stdout
    assert "Offline plan only" in result.stdout


def test_native_waf_uses_one_launcher_and_the_maintained_tree(tmp_path):
    scripts = tmp_path / 'scripts'
    scripts.mkdir()
    launcher = scripts / 'build-safe.sh'
    launcher.write_text('''#!/bin/bash
set -eu
[[ ${TEST_LAUNCHER_ACTIVE:-0} == 0 ]] || exit 91
export TEST_LAUNCHER_ACTIVE=1
[[ $1 == run && $2 == -- ]]
shift 2
exec "$@"
''')
    launcher.chmod(0o700)
    interpreter = tmp_path / 'fake-python'
    interpreter.write_text('#!/bin/sh\nprintf "NATIVE_ARG=%s\\n" "$@"\n')
    interpreter.chmod(0o700)
    code = DEFINITIONS + '\n' + '\n'.join([
        'ROOT=' + shlex.quote(str(tmp_path)),
        'NATIVE_BUILD_DIR="$ROOT/build"',
        'BUILD_DIR="$ROOT/external-sdk"',
        'PYTHON_BIN=' + shlex.quote(str(interpreter)),
        'refresh_global_pkg_config_path() { :; }',
        # shell() normally replaces the bounded launcher with a command stub.
        'run_bounded() { bash "$ROOT/scripts/build-safe.sh" run -- "$@"; }',
        'run_waf_clean build --targets=unit-tests',
    ])
    result = shell(code)
    assert result.returncode == 0, result.stderr
    assert 'NATIVE_ARG=--out=' + str(tmp_path / 'build') in result.stdout
    assert 'NATIVE_ARG=--targets=unit-tests' in result.stdout


def test_default_sdk_scratch_reuses_one_purpose_and_releases_after_success(tmp_path):
    source = tmp_path / 'source'
    source.mkdir()
    code = DEFINITIONS + '\nROOT=' + shlex.quote(str(source)) + '''
BUILD_DIR=""
REUSE_BUILD_DIR=0
prepare_build_dir
first="$BUILD_DIR"
printf 'FIRST=%s\\n' "$first"
BUILD_DIR=""
REUSE_BUILD_DIR=0
prepare_build_dir
printf 'SECOND=%s\\n' "$BUILD_DIR"
[[ "$first" == "$BUILD_DIR" ]]
release_default_sdk_scratch
[[ ! -e "$first" ]]
'''
    result = shell(code)
    assert result.returncode == 0, result.stderr
    lines = result.stdout.splitlines()
    first = next(line.split('=', 1)[1] for line in lines if line.startswith('FIRST='))
    second = next(line.split('=', 1)[1] for line in lines if line.startswith('SECOND='))
    assert first == second
    assert first.startswith('/tmp/ndnsf-stack-sdk-')
    assert not Path(first).exists()


def test_default_sdk_scratch_does_not_remove_declared_installation(tmp_path):
    source = tmp_path / 'source'
    source.mkdir()
    code = DEFINITIONS + '\nROOT=' + shlex.quote(str(source)) + '''
BUILD_DIR=""
REUSE_BUILD_DIR=0
prepare_build_dir
original_prefix="$RUNTIME_PREFIX"
RUNTIME_PREFIX="$BUILD_DIR/retained-install"
mkdir -p -- "$RUNTIME_PREFIX"
release_default_sdk_scratch
[[ -d "$RUNTIME_PREFIX" ]]
RUNTIME_PREFIX="$original_prefix"
release_default_sdk_scratch
[[ ! -e "$BUILD_DIR" ]]
'''
    result = shell(code)
    assert result.returncode == 0, result.stderr
    assert 'Retaining SDK scratch containing an explicit input/install path' in result.stdout


def test_core_does_not_probe_or_download_model_sdk():
    result = shell(DEFINITIONS + '''
has_onnxruntime_sdk() { echo UNEXPECTED_MODEL_PROBE; exit 99; }
install_host_sdk_prerequisites
''', "--components", "core")
    assert result.returncode == 0, result.stderr
    assert not result.stdout


@pytest.mark.parametrize("already_installed", [False, True])
def test_di_os_packages_exclude_gui(already_installed):
    result = shell(DEFINITIONS + '''
sudo_run() { printf 'SUDO:%s\\n' "$*"; }
dpkg-query() { [[ "$NDNSF_TEST_INSTALLED" == 1 ]] && printf 'install ok installed'; }
has_global_boost() { return 0; }
select_global_boost() { return 0; }
install_common_system_packages
''', "--components", "di", env=dict(os.environ, NDNSF_TEST_INSTALLED=str(int(already_installed))))
    assert result.returncode == 0, result.stderr
    if already_installed:
        assert "SUDO:" not in result.stdout
    else:
        assert "protobuf-compiler" in result.stdout
        assert "rsync" in result.stdout
        assert "unzip" in result.stdout
    assert "gtkmm" not in result.stdout
    assert "gstreamer" not in result.stdout


def test_uav_os_packages_include_native_video_codecs():
    result = shell(DEFINITIONS + '''
sudo_run() { printf 'SUDO:%s\\n' "$*"; }
dpkg-query() { return 1; }
has_global_boost() { return 0; }
select_global_boost() { return 0; }
install_common_system_packages
''', "--components", "all")
    assert result.returncode == 0, result.stderr
    for package in ("gstreamer1.0-plugins-good", "gstreamer1.0-plugins-bad",
                    "gstreamer1.0-libav"):
        assert package in result.stdout
    for package in ("libopencv-dev", "python3-aiohttp", "ffmpeg"):
        assert package not in result.stdout


@pytest.mark.parametrize("profile", ["core", "repo", "di", "all"])
def test_main_checks_model_sdk_lock_only_when_selected(profile):
    stubs = '''
validate_host_sdk_lock() { echo UNAVAILABLE_MODEL_SDK_LOCK >&2; return 91; }
'''
    result = shell(DEFINITIONS + stubs + '\ncd "$ROOT"\n' + MAIN,
                   "--source", "--plan", "--components", profile)
    if profile in ("core", "repo"):
        assert result.returncode == 0, result.stderr
        assert "UNAVAILABLE_MODEL_SDK_LOCK" not in result.stderr
        assert "ONNX Runtime" not in result.stdout
    else:
        assert result.returncode == 91
        assert "UNAVAILABLE_MODEL_SDK_LOCK" in result.stderr


@pytest.mark.parametrize("prior", [None, [], {"schema": 1, "libraries": {
    "libonnxruntime.so": {"unselected": "preserve exactly"}}}])
def test_core_receipt_refresh_preserves_only_valid_prior_metadata(tmp_path, prior):
    import hashlib
    import json
    import shutil
    library = tmp_path / "lib"
    library.mkdir()
    names = ("libndn-cxx.so", "libndnsd.so", "libndn-svs.so", "libnac-abe.so", "libopenabe.so")
    for name in names:
        shutil.copyfile("/usr/bin/true", library / name)
    receipt = tmp_path / "prior.json"
    receipt.write_text(json.dumps(prior))
    result = shell(DEFINITIONS + '''
GLOBAL_LIBRARY_DIR="$NDNSF_TEST_LIBDIR"
GLOBAL_IDENTITY_FILE="$NDNSF_TEST_RECEIPT"
pkg_config() {
  [[ "$*" != *onnxruntime* ]] || { touch "$NDNSF_TEST_MODEL_QUERY"; return 91; }
  printf '%s' "$NDNSF_TEST_LIBDIR"
}
global_dependency_identity_receipt
''', "--components", "core", env=dict(os.environ, NDNSF_TEST_LIBDIR=str(library),
                                       NDNSF_TEST_RECEIPT=str(receipt),
                                       NDNSF_TEST_MODEL_QUERY=str(tmp_path / "model-query")))
    assert result.returncode == 0, result.stderr
    assert not (tmp_path / "model-query").exists()
    actual = json.loads(result.stdout)
    assert actual["schema"] == 1
    for name in names:
        assert actual["libraries"][name]["sha256"] == hashlib.sha256((library / name).read_bytes()).hexdigest()
    if isinstance(prior, dict):
        assert actual["libraries"]["libonnxruntime.so"] == prior["libraries"]["libonnxruntime.so"]
    else:
        assert set(actual["libraries"]) == set(names)


@pytest.mark.parametrize("args", [
    ["--prefix", "relative"], ["--prefix", "/"], ["--prefix", "/dev/shm/runtime"],
    ["--prefix", str(ROOT / "runtime")], ["--", "--prefix=/opt/other"],
    ["--", "--libdir", "/opt/other/lib"],
    ["--prefix", "/opt/ndnsf runtime"], ["--prefix", "/opt/ndnsf\truntime"],
    ["--prefix", "/opt/ndnsf,runtime"],
    ["--prefix", "/opt/ndnsf'"], ["--prefix", '/opt/ndnsf"'],
    ["--prefix", "/opt/ndnsf\\runtime"], ["--prefix", "/opt/ndnsf#runtime"],
])
def test_bad_runtime_destination_stops_before_mutation(args):
    result = shell(SCRIPT.read_text(), "--plan", *args)
    assert result.returncode == 2
    assert "NDNSF stack install root" not in result.stdout
    assert not result.stdout


def test_custom_prefix_manifest_restores_only_owned_files(tmp_path):
    prefix = tmp_path / "runtime"
    stage = tmp_path / "stage"
    (stage / "bin").mkdir(parents=True)
    (stage / "bin/service").write_text("new")
    (prefix / "bin").mkdir(parents=True)
    (prefix / "bin/service").write_text("prior")
    persistent = prefix / "repo/packet"
    persistent.parent.mkdir()
    persistent.write_text("preserved")
    result = shell(DEFINITIONS + '''
sudo_run() { "$@"; }
install_manifest_tree "$NDNSF_TEST_STAGE" "$RUNTIME_PREFIX" ndnsf-native
test "$(cat "$RUNTIME_PREFIX/bin/service")" = new
manifest_tool uninstall --manifest "$INSTALL_MANIFEST"
''', "--prefix", str(prefix), env=dict(os.environ, NDNSF_TEST_STAGE=str(stage)))
    assert result.returncode == 0, result.stderr
    assert (prefix / "bin/service").read_text() == "prior"
    assert persistent.read_text() == "preserved"


def test_private_test_python_uses_the_selected_manifest_root(tmp_path):
    prefix = tmp_path / 'runtime'
    scratch = tmp_path / 'scratch'
    scratch.mkdir()
    result = shell(DEFINITIONS + '''
sudo_run() { "$@"; }
env() {
  if [[ "${*: -1}" == 'import cryptography' ]]; then return 0; fi
  command env "$@"
}
NDNSF_INSTALL_DI=1
WAF_CONFIGURE_ARGS=(--with-tests)
run_bounded() {
  local destination=
  while (($#)); do
    if [[ "$1" == --target ]]; then destination="$2"; break; fi
    shift
  done
  [[ -n "$destination" ]] || return 98
  mkdir -p "$destination/ndn"
  printf '# private test namespace\n' > "$destination/ndn/__init__.py"
}
install_native_test_python_support
''', '--prefix', str(prefix), env=dict(os.environ, TMPDIR=str(scratch)))
    assert result.returncode == 0, result.stderr
    target = prefix / 'libexec/ndnsf-tests/python/ndn/__init__.py'
    assert target.read_text() == '# private test namespace\n'
    import json
    receipt = json.loads((prefix / 'share/ndnsf/stack-install-manifest.json').read_text())
    assert receipt['files'][str(target)]['owners'] == ['ndnsf-test-python']
    assert not list(scratch.iterdir())


def test_runtime_prefix_cannot_be_an_existing_file(tmp_path):
    prefix = tmp_path / "file"
    prefix.write_text("preserved")
    result = shell(SCRIPT.read_text(), "--plan", "--prefix", str(prefix))
    assert result.returncode == 2
    assert not result.stdout
    assert prefix.read_text() == "preserved"


def test_python_wheel_staging_excludes_runtime_and_build_artifacts(tmp_path):
    assert os.geteuid() != 0, "run this permission oracle without root"
    source = tmp_path / "source"
    stage = tmp_path / "stage"
    packages = ["pythonWrapper", "NDNSF-DistributedRepo/pythonWrapper"] + [
        "NDNSF-DistributedInference/packaging/python/" + name
        for name in ("core", "sdk", "planner", "app", "ops", "compat")
    ] + ["extra/nested/package"]
    retained = [
        "config.hpp", "ndn-service-framework/NetworkProbeRate.hpp",
        "NDNSF-DistributedRepo/include/repo.hpp",
        "NDNSF-DistributedRepo/src/repo.cpp",
        "NDNSF-DistributedInference/cpp/probe.cpp",
        "NDNSF-DistributedInference/ndnsf_di_launcher.py",
        "NDNSF-DistributedInference/ndnsf_distributed_inference/compatibility/manifest.json",
        "packaging/ndnsf-di-container/lib/allocation_topology.py",
        "packaging/ndnsf-di-container/lib/adapters/__init__.py",
        "packaging/ndnsf-di-container/lib/adapters/base.py",
        "packaging/ndnsf-di-container/lib/adapters/slurm_apptainer.py",
        "Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-allocation-topology.sh",
        "Experiments/TigerCluster/adapters/slurm-apptainer/scripts/configure-allocation-routes.sh",
        "Experiments/TigerCluster/adapters/slurm-apptainer/scripts/run-container.sh",
        "Experiments/TigerCluster/adapters/slurm-apptainer/scripts/stage-sif.sh",
        "Experiments/TigerCluster/adapters/slurm-apptainer/scripts/validate-local-sif-build-record.py",
        "pythonWrapper/ndnsf/py.typed", "pythonWrapper/src/ndnsf/_ndnsf.cpp",
        "extra/nested/package/assets/weights.bin",
    ]
    excluded = [
        "build/integration-tests", "build-system-j2/object.o", "build-spec197/object.o",
        "pythonWrapper/build/object.o", "pythonWrapper/ndnsf/__pycache__/cache.pyc",
        ".git/HEAD", "pythonWrapper/nested/.git/HEAD", "pythonWrapper/ndnsf.egg-info/PKG-INFO",
        "docs/report.pdf", "specs/tasks.md", "results/private/output.json",
        ".scratch/run/model.onnx", ".index-store/index.db", ".agent-rules/skill.md",
        ".feature/feature.json", "models/weights.bin", "dependencies/SDK.hpp",
        "pythonWrapper/ndnsf/_ndnsf.so", "pythonWrapper/ndnsf/_ndnsf.so.1",
        "NDNSF-DistributedRepo/librepo.a", "NDNSF-DistributedRepo/object.o",
        "pythonWrapper/ndnsf/cache.pyo",
        "unrequested-model.onnx", "other-experiment/weights.bin",
        "extra/unrequested/weights.bin",
        "packaging/ndnsf-di-container/lib/unrequested-private.json",
        "packaging/ndnsf-di-container/lib/adapters/docker_compose.py",
        "packaging/ndnsf-di-container/jobs/private/credentials.key",
    ]
    for name in packages:
        retained.extend((name + "/setup.py", name + "/package_source.py"))
    for name in retained + excluded:
        path = source / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(name)
    restricted = source / "results"
    restricted.chmod(0)
    try:
        env = dict(os.environ, NDNSF_TEST_ROOT=str(source), NDNSF_TEST_STAGE=str(stage))
        result = shell(DEFINITIONS + '''
ROOT="$NDNSF_TEST_ROOT"
stage_python_wheel_sources "$NDNSF_TEST_STAGE" ''' + " ".join(
            '"$ROOT/' + name + '"' for name in packages
        ) + '''
printf '%s\\n' "${PYTHON_WHEEL_INPUTS[@]}"
''', env=env)
        assert result.returncode == 0, result.stderr
        copied = stage / "repository"
        assert result.stdout.splitlines() == [str(copied / name) for name in packages]
        for name in retained:
            assert (copied / name).read_text() == name
        for name in excluded:
            assert not (copied / name).exists(), name
    finally:
        restricted.chmod(0o700)


def test_python_wheel_staging_preserves_external_package_source(tmp_path):
    source = tmp_path / "outside"
    source.mkdir()
    (source / "pyproject.toml").write_text("[build-system]\n")
    (source / "package.py").write_text("# external source\n")
    (source / "build").mkdir()
    (source / "build" / "object.o").write_text("generated")
    stage = tmp_path / "stage"
    env = dict(os.environ, NDNSF_TEST_ROOT=str(tmp_path / "repo"),
               NDNSF_TEST_STAGE=str(stage), NDNSF_TEST_EXTERNAL=str(source))
    result = shell(DEFINITIONS + '''
ROOT="$NDNSF_TEST_ROOT"
stage_python_wheel_sources "$NDNSF_TEST_STAGE" "$NDNSF_TEST_EXTERNAL"
printf '%s\\n' "${PYTHON_WHEEL_INPUTS[@]}"
''', env=env)
    assert result.returncode == 0, result.stderr
    copied = stage / "external" / "0" / "outside"
    assert result.stdout.strip() == str(copied)
    assert (copied / "package.py").read_text() == "# external source\n"
    assert not (copied / "build").exists()


@pytest.mark.parametrize("version, succeeds", [("20.04", True), ("26.04", False)])
def test_missing_host_helper_uses_only_established_boost_profile(tmp_path, version, succeeds):
    env = dict(os.environ, NDNSF_TEST_ROOT=str(tmp_path), NDNSF_TEST_HOST=version)
    result = shell(DEFINITIONS + '''
ROOT="$NDNSF_TEST_ROOT"
HOST_UBUNTU_VERSION="$NDNSF_TEST_HOST"
select_global_boost
printf '%s\t%s\t%s\t%s' "$BOOST_INCLUDE_DIR" "$BOOST_LIBRARY_DIR" "$BOOST_VERSION_NUMBER" "$BOOST_VERSION_STRING"
''', env=env)
    if succeeds:
        assert result.returncode == 0, result.stderr
        assert result.stdout == "/usr/include\t/usr/lib/x86_64-linux-gnu\t107100\t1.71.0"
    else:
        assert result.returncode != 0
        assert "Missing scripts/host_profile.py for Ubuntu 26.04" in result.stderr
        assert result.stdout == ""


def test_qwen_profile_plan_is_offline_and_enables_installed_examples():
    result = shell(SCRIPT.read_text(), "--with-qwen-minindn", "--plan")
    assert result.returncode == 0, result.stderr
    for flag in ("--with-examples", "--with-tests", "--install-experiment-fixtures"):
        assert flag in result.stdout
    assert "no download, export, quantization, or inference" in result.stdout
    assert "NDNSF stack install root" not in result.stdout


@pytest.mark.parametrize("mode", ["--deps-only", "--configure-only", "--check-dependencies", "--no-system-install"])
def test_qwen_profile_rejects_partial_install(mode):
    result = shell(SCRIPT.read_text(), "--with-qwen-minindn", mode, '--plan')
    assert result.returncode == 2
    assert "requires a complete system installation" in result.stderr


def test_qwen_prerequisite_failure_stops_before_system_packages():
    stubs = '''
require_host_profile() { :; }
check_qwen_minindn_prerequisites() { echo PREREQUISITE_FAILURE >&2; return 1; }
install_common_system_packages() { echo UNEXPECTED_APT; }
'''
    result = shell(DEFINITIONS + stubs + MAIN, "--with-qwen-minindn")
    assert result.returncode != 0
    assert "PREREQUISITE_FAILURE" in result.stderr
    assert "UNEXPECTED_APT" not in result.stdout


@pytest.mark.parametrize(
    "mode, expected, forbidden",
    [
        ("--with-qwen-minindn", ("libgtest-dev",),
         ("doxygen", "graphviz", "mininet", "openvswitch-switch", "tcpdump")),
        ("--with-minindn-deps", ("mininet", "openvswitch-switch", "tcpdump"),
         ("libgtest-dev", "doxygen", "graphviz")),
        ("--with-system-tests-deps", ("libgtest-dev", "doxygen", "graphviz"), ()),
    ],
)
def test_profiles_install_only_selected_test_and_doc_packages(
    tmp_path, mode, expected, forbidden
):
    fake_bin = tmp_path / "bin"
    fake_bin.mkdir()
    for name in ("apt-get", "dpkg-query"):
        tool = fake_bin / name
        tool.write_text("#!/bin/sh\nexit 1\n")
        tool.chmod(0o755)
    env = dict(os.environ, PATH=f"{fake_bin}:{os.environ['PATH']}")
    stubs = '''
sudo_run() { printf 'SUDO:%s\\n' "$*"; }
select_global_boost() { return 0; }
install_common_system_packages
'''
    result = shell(DEFINITIONS + stubs, mode, env=env)
    assert result.returncode == 0, result.stderr
    install = next(line for line in result.stdout.splitlines() if "apt-get install" in line)
    for package in expected:
        assert package in install
    for package in forbidden:
        assert package not in install


def test_qwen_program_check_rejects_missing_binary(tmp_path):
    result = shell(DEFINITIONS + '\nRUNTIME_PREFIX=' + repr(str(tmp_path)) + '\ncheck_qwen_installed_programs')
    assert result.returncode != 0
    assert "Missing installed Qwen executable" in result.stderr


def test_qwen_program_check_does_not_claim_inference(tmp_path):
    for name in ("bin/App_ServiceController", "bin/DI_NativeArtifactAuthority", "bin/DI_NativeRequester",
                 "bin/di-native-provider", "libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker", "bin/multiturn-oracle"):
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#!/bin/sh\nexit 0\n")
        path.chmod(0o755)
    result = shell(DEFINITIONS + '\nRUNTIME_PREFIX=' + repr(str(tmp_path)) + '\ncheck_qwen_installed_programs')
    assert result.returncode == 0, result.stderr
    assert "NOT an inference PASS" in result.stdout


def test_qwen_program_check_rejects_executable_directory(tmp_path):
    names = (
        "bin/App_ServiceController", "bin/DI_NativeArtifactAuthority", "bin/DI_NativeRequester",
        "bin/di-native-provider", "libexec/ndnsf-di/DI_NativeOnnxAssemblyWorker",
        "bin/multiturn-oracle",
    )
    directory = tmp_path / names[0]
    directory.mkdir(parents=True)
    directory.chmod(0o755)
    for name in names[1:]:
        path = tmp_path / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("#!/bin/sh\nexit 0\n")
        path.chmod(0o755)
    result = shell(
        DEFINITIONS + '\nRUNTIME_PREFIX=' + repr(str(tmp_path))
        + '\ncheck_qwen_installed_programs'
    )
    assert result.returncode != 0
    assert "Missing installed Qwen executable" in result.stderr
    assert names[0] in result.stderr


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
check_waf_inventory() { :; }
install_common_system_packages() { echo UNEXPECTED_APT; }
build_waf_dependency() { echo UNEXPECTED_BUILD; }
install_external_dependencies
''')
    assert result.returncode == 31
    assert "UNEXPECTED" not in result.stdout


@pytest.mark.parametrize('initial', ['clean', 'preapplied', 'incompatible'])
@pytest.mark.parametrize('fail_build', [False, True])
def test_waf_dependency_patch_lifecycle_and_quoted_source_path(tmp_path, initial, fail_build):
    source = tmp_path / "source ' with spaces" / "ndn-svs"
    source.mkdir(parents=True)
    original = {'clean': 'original\n', 'preapplied': 'updated\n',
                'incompatible': 'unrelated\n'}[initial]
    (source / 'value.txt').write_text(original)
    project = tmp_path / 'project'
    patch = project / 'tools/ndn-svs/publication-latency.patch'
    patch.parent.mkdir(parents=True)
    patch.write_text('diff --git a/value.txt b/value.txt\n'
                     '--- a/value.txt\n+++ b/value.txt\n'
                     '@@ -1 +1 @@\n-original\n+updated\n')
    waf = source / "waf"
    waf.write_text('import os, sys\nfrom pathlib import Path\n'
                   'assert Path(__file__).with_name("value.txt").read_text() == "updated\\n"\n'
                   'print("FAKE_WAF:" + os.getcwd() + ":" + " ".join(sys.argv[1:]))\n'
                   'if "build" in sys.argv and os.environ["NDNSF_TEST_FAIL_BUILD"] == "1":\n'
                   '    raise SystemExit(19)\n'
                   'if "install" in sys.argv:\n'
                   '    stage = next(a.split("=", 1)[1] for a in sys.argv if a.startswith("--destdir="))\n'
                   '    target = Path(stage + os.environ["NDNSF_TEST_PREFIX"]) / "lib/test-file"\n'
                   '    target.parent.mkdir(parents=True)\n'
                   '    target.write_text("payload")\n')
    prefix = tmp_path / 'prefix'
    env = dict(os.environ, NDNSF_DISK_RESERVE_BYTES='0', NDNSF_TEST_PREFIX=str(prefix),
               NDNSF_TEST_ROOT=str(project), NDNSF_TEST_BUILD=str(tmp_path / 'build'),
               NDNSF_TEST_SOURCE=str(source), NDNSF_TEST_FAIL_BUILD=str(int(fail_build)))
    result = shell(DEFINITIONS + '''
ROOT="$NDNSF_TEST_ROOT"
BUILD_DIR="$NDNSF_TEST_BUILD"
GLOBAL_DEPENDENCY_PREFIX="$NDNSF_TEST_PREFIX"
FORCE_DEPENDENCIES=1
sudo_run() { :; }
ensure_source_tree() { printf '%s\n' "$NDNSF_TEST_SOURCE"; }
install_manifest_tree() { mkdir -p "$2"; cp -a "$1/." "$2"; echo STAGED_INSTALL; }
record_source_receipt() { echo SOURCE_RECEIPT; }
build_waf_dependency ndn-svs libndn-svs invalid-url 0.1
''', "--deps-dir", str(source.parent), "--jobs", "2", env=env)
    expected = 1 if initial == 'incompatible' else 19 if fail_build else 0
    assert result.returncode == expected, result.stderr
    assert (source / 'value.txt').read_text() == original
    if initial == 'incompatible':
        assert 'FAKE_WAF' not in result.stdout
        return
    output = tmp_path / 'build/waf-dependencies/ndn-svs'
    assert f"configure --prefix={prefix}" in result.stdout
    assert f"FAKE_WAF:{output}:" in result.stdout
    assert f"--out={output}" in result.stdout
    assert " build -j2" in result.stdout
    assert "distclean" not in result.stdout
    assert ('STAGED_INSTALL' in result.stdout) == (not fail_build)
    assert ('SOURCE_RECEIPT' in result.stdout) == (not fail_build)
    install_call = f"install --destdir={output / 'install-stage'}"
    assert (install_call in result.stdout) == (not fail_build)
    assert not (output / 'install-stage').exists()


@pytest.mark.parametrize("tracking_installed", [True, False])
def test_install_refreshes_loader_before_python_and_limits_jobs(tmp_path, tracking_installed):
    prefix = tmp_path / 'runtime'
    library_dir = prefix / 'lib'
    library_dir.mkdir(parents=True)
    for name in ('libndn-service-framework.so', 'libndnsf-distributed-inference.so',
                 'libndnsf-distributed-repo.a'):
        (library_dir / name).touch()
    if tracking_installed:
        for name in ('bin/UavTrackingNode', 'lib/libndnsf-uav-tracking.so',
                     'lib/pkgconfig/ndnsf-uav-tracking.pc',
                     'include/ndnsf/uav/UavObservationSource.hpp'):
            artifact = prefix / name
            artifact.parent.mkdir(parents=True, exist_ok=True)
            artifact.touch()
    stubs = '''
PYTHON_BIN=/bin/echo
select_nac_sdk() { NAC_SDK_PREFIX=/usr/local/ndnsf-sdk/nac-abe/fixture; }
require_system_toolchain() { :; }
require_host_profile() { :; }
install_common_system_packages() { :; }
check_waf_inventory() { :; }
require_global_external_closure() { :; }
source_field() { printf '%s\n' 'https://example.invalid/test.git'; }
require_global_file() { :; }
prepare_build_dir() { :; }
lock_native_install() { :; }
native_stage() { :; }
install_manifest_tree() { :; }
run_waf_clean() { echo "WAF:$*"; echo "NDNSF_SKIP_DEV_PIP_INSTALL=${NDNSF_SKIP_DEV_PIP_INSTALL:-0}"; }
sudo_run() { echo "SUDO:$*"; }
native_digest_receipt() { echo '{}'; }
pip_install() { echo "PIP:$*"; }
run() { printf 'RUN:%s\\n' "$*"; }
'''
    stubs = ('RUNTIME_PREFIX=' + shlex.quote(str(prefix)) + '\n' +
             'GLOBAL_DEPENDENCY_PREFIX=' + shlex.quote(str(prefix)) + '\n' +
             'RUNTIME_LIBRARY_DIR=' + shlex.quote(str(library_dir)) + '\n' + stubs)
    result = shell(DEFINITIONS + stubs + MAIN, "--no-dependencies", "--jobs", "2")
    if not tracking_installed:
        assert result.returncode == 1
        assert 'Installed tracking SDK file missing:' in result.stderr
        assert 'PIP:' not in result.stdout
        return
    assert result.returncode == 0, result.stderr
    assert "WAF:-j2" in result.stdout
    install = next(shlex.split(line[4:]) for line in result.stdout.splitlines()
                   if line.startswith('WAF:install '))
    assert '-j2' in install
    assert any(token.startswith('--destdir=') for token in install)
    assert "NDNSF_SKIP_DEV_PIP_INSTALL=1" in result.stdout
    assert 'packaging/python/core' in result.stdout
    assert 'packaging/python/compat' in result.stdout
    assert result.stdout.count('PIP:') == 1
    assert result.stdout.index("SUDO:/sbin/ldconfig") < result.stdout.index("PIP:")
    assert 'check-runtime' in result.stdout
    assert '--prefix /usr/local/ndnsf-sdk/nac-abe/fixture --require-nac' in result.stdout


def test_build_only_does_not_install_python():
    stubs = '''
require_system_toolchain() { :; }
require_host_profile() { :; }
install_common_system_packages() { :; }
check_waf_inventory() { :; }
require_global_external_closure() { :; }
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
    assert 'host SDKs pinned in packaging/host-sdk-dependencies.lock.json' in result.stdout
    assert 'Configure-only mode requires' in result.stdout
    assert not (tmp_path / 'absent').exists()


def test_configure_modes_conflict_before_side_effects():
    for args in [('--configure-only', '--source'), ('--no-install', '--deps-only'),
                 ('--check-dependencies', '--source')]:
        result = shell(SCRIPT.read_text(), *args, '--plan')
        assert result.returncode == 2
        assert 'cannot be combined' in result.stderr


def test_force_and_no_dependencies_conflict_before_side_effects():
    for args in [('--force-dependencies', '--no-dependencies'),
                 ('--no-dependencies', '--force-dependencies')]:
        result = shell(SCRIPT.read_text(), *args, '--plan')
        assert result.returncode == 2
        assert 'cannot be combined' in result.stderr


def test_deps_only_stops_before_waf_and_python():
    stubs = '''
require_system_toolchain() { :; }
require_host_profile() { :; }
install_common_system_packages() { :; }
check_waf_inventory() { :; }
require_global_external_closure() { return 1; }
source_field() { echo https://example.invalid/repo.git; }
install_external_dependencies() { echo DEPS_ONLY_OK; }
run_waf_clean() { echo UNEXPECTED_WAF; }
pip_install() { echo UNEXPECTED_PIP; }
'''
    result = shell(DEFINITIONS + stubs + MAIN, '--source', '--deps-only')
    assert result.returncode == 0, result.stderr
    assert 'DEPS_ONLY_OK' in result.stdout
    assert 'UNEXPECTED' not in result.stdout


@pytest.mark.parametrize('source_args', [(), ('--source',)])
def test_default_uses_per_dependency_resolver_without_closure_identity_gate(source_args):
    stubs = '''
require_host_profile() { :; }
install_common_system_packages() { :; }
require_system_toolchain() { :; }
source_field() { echo https://example.invalid/repo.git; }
require_global_external_closure() { echo UNEXPECTED_WHOLE_CLOSURE_GATE; return 1; }
check_waf_inventory() { echo UNEXPECTED_WAF_INVENTORY_GATE; return 1; }
install_external_dependencies() { echo PER_DEPENDENCY_RESOLVER; }
'''
    result = shell(DEFINITIONS + stubs + MAIN, *source_args, '--deps-only')
    assert result.returncode == 0, result.stderr
    assert 'PER_DEPENDENCY_RESOLVER' in result.stdout
    assert 'UNEXPECTED_WHOLE_CLOSURE_GATE' not in result.stdout
    assert 'UNEXPECTED_WAF_INVENTORY_GATE' not in result.stdout


def test_force_dependencies_routes_to_dependency_resolver():
    stubs = '''
require_host_profile() { :; }
install_common_system_packages() { :; }
require_system_toolchain() { :; }
source_field() { echo https://example.invalid/repo.git; }
require_global_external_closure() { echo UNEXPECTED_REUSE; }
check_waf_inventory() { :; }
install_external_dependencies() { echo SOURCE_INSTALL; }
'''
    result = shell(DEFINITIONS + stubs + MAIN, '--force-dependencies', '--deps-only')
    assert result.returncode == 0, result.stderr
    assert 'SOURCE_INSTALL' in result.stdout
    assert 'UNEXPECTED_REUSE' not in result.stdout


def test_no_dependencies_reuses_global_closure_without_source_receipts():
    stubs = '''
require_host_profile() { :; }
install_common_system_packages() { :; }
require_system_toolchain() { :; }
source_field() { echo https://example.invalid/repo.git; }
require_global_external_closure() { echo GLOBAL_CLOSURE_OK; }
check_waf_inventory() { echo WAF_INVENTORY_OK; }
require_source_receipts() { echo UNEXPECTED_SOURCE_RECEIPT_CHECK; return 99; }
install_external_dependencies() { echo UNEXPECTED_SOURCE_INSTALL; }
'''
    result = shell(DEFINITIONS + stubs + MAIN, '--no-dependencies', '--deps-only')
    assert result.returncode == 0, result.stderr
    assert 'GLOBAL_CLOSURE_OK' in result.stdout
    assert 'WAF_INVENTORY_OK' in result.stdout
    assert 'UNEXPECTED' not in result.stdout


def test_check_dependencies_only_requires_verified_installed_closure():
    stubs = '''
require_host_profile() { :; }
require_system_toolchain() { :; }
source_field() { echo https://example.invalid/repo.git; }
check_waf_inventory() { echo WAF_INVENTORY_OK; }
require_global_external_closure() { echo GLOBAL_CLOSURE_OK; }
require_source_receipts() { echo UNEXPECTED_SOURCE_RECEIPT_CHECK; return 99; }
install_common_system_packages() { echo UNEXPECTED_APT; }
install_external_dependencies() { echo UNEXPECTED_SOURCE_INSTALL; }
'''
    result = shell(DEFINITIONS + stubs + MAIN, '--check-dependencies')
    assert result.returncode == 0, result.stderr
    assert 'GLOBAL_CLOSURE_OK' in result.stdout
    assert 'WAF_INVENTORY_OK' in result.stdout
    assert 'Installed NDNSF global dependency closure is valid' in result.stdout
    assert 'UNEXPECTED' not in result.stdout


def test_compatible_package_skips_without_source_receipt():
    result = shell(DEFINITIONS + '''
is_pkg_installed() { return 0; }
has_source_receipt() { echo RECEIPT_CHECK_SHOULD_NOT_RUN; return 1; }
ensure_source_tree() { echo SOURCE_REQUIRED >&2; return 23; }
build_waf_dependency ndn-svs libndn-svs unused 0.1
''')
    assert result.returncode == 0
    assert 'already installed' in result.stdout
    assert 'RECEIPT_CHECK_SHOULD_NOT_RUN' not in result.stdout
    assert 'SOURCE_REQUIRED' not in result.stderr


def test_incompatible_package_builds_without_source_receipt():
    result = shell(DEFINITIONS + '''
is_pkg_installed() { return 1; }
ensure_source_tree() { echo SOURCE_REQUIRED >&2; return 23; }
build_waf_dependency ndn-svs libndn-svs unused 0.1
''')
    assert result.returncode == 23
    assert 'SOURCE_REQUIRED' in result.stderr


@pytest.mark.parametrize('builder,detector', [
    ('build_waf_dependency ndn-svs libndn-svs unused 0.1', 'is_pkg_installed'),
])
def test_all_dependency_builders_reuse_compatible_install_without_receipt(builder, detector):
    detector_stub = f'{detector}() {{ return 0; }}'
    result = shell(DEFINITIONS + '''
''' + detector_stub + '''
has_source_receipt() { echo RECEIPT_CHECK_SHOULD_NOT_RUN; return 1; }
ensure_source_tree() { echo SOURCE_REQUIRED >&2; return 23; }
''' + builder + '\n')
    assert result.returncode == 0
    assert 'already installed' in result.stdout
    assert 'RECEIPT_CHECK_SHOULD_NOT_RUN' not in result.stdout
    assert 'SOURCE_REQUIRED' not in result.stderr


def test_nac_version_match_cannot_skip_required_patched_sdk():
    result = shell(DEFINITIONS + '''
NAC_SDK_PREFIX=/usr/local/ndnsf-sdk/nac-abe/fixture
is_pkg_installed() { echo OLD_VERSION_MATCH; return 0; }
verify_nac_sdk() { echo PATCHED_RECEIPT_REJECTED; return 1; }
ensure_source_tree() { echo SOURCE_REQUIRED >&2; return 23; }
build_cmake_dependency NAC-ABE libnac-abe unused 0.1
''')
    assert result.returncode == 23
    assert 'PATCHED_RECEIPT_REJECTED' in result.stdout
    assert 'SOURCE_REQUIRED' in result.stderr
    assert 'already installed' not in result.stdout


def test_nac_verified_sdk_reuses_without_touching_checkout():
    result = shell(DEFINITIONS + '''
NAC_SDK_PREFIX=/usr/local/ndnsf-sdk/nac-abe/fixture
verify_nac_sdk() { return 0; }
ensure_source_tree() { echo UNEXPECTED_CHECKOUT >&2; return 23; }
build_cmake_dependency NAC-ABE libnac-abe unused 0.1
''')
    assert result.returncode == 0
    assert 'Locked patched NAC SDK already installed' in result.stdout
    assert 'UNEXPECTED_CHECKOUT' not in result.stderr


def test_selected_nac_sdk_reaches_controlled_pkg_config_and_waf(tmp_path):
    sdk = tmp_path / 'sdk with spaces'
    (sdk / 'lib/pkgconfig').mkdir(parents=True)
    probe = tmp_path / 'python-argv-probe'
    probe.write_text('#!/bin/sh\nprintf "<%s>\\n" "$@"\n')
    probe.chmod(0o755)
    env = dict(os.environ, NDNSF_TEST_SDK=str(sdk), NDNSF_TEST_PYTHON=str(probe))
    result = shell(DEFINITIONS + '''
NAC_SDK_PREFIX="$NDNSF_TEST_SDK"
refresh_global_pkg_config_path
printf 'CONTROLLED_PATH=%s\n' "$GLOBAL_PKG_CONFIG_PATH"
PYTHON_BIN="$NDNSF_TEST_PYTHON"
run_waf_clean configure
check_waf_inventory
''', env=env)
    assert result.returncode == 0
    assert f'CONTROLLED_PATH={sdk}/lib/pkgconfig:' in result.stdout
    assert f'<--nac-abe-prefix={sdk}>' in result.stdout
    assert f'<--nac-abe-prefix>\n<{sdk}>' in result.stdout


def test_configure_only_requires_patched_sdk_before_system_changes():
    stubs = '''
source_field() { echo https://example.invalid/repo.git; }
select_nac_sdk() { echo SDK_SELECTED; NAC_SDK_PREFIX=/usr/local/ndnsf-sdk/nac-abe/fixture; }
verify_nac_sdk() { echo SDK_REJECTED; return 1; }
require_host_profile() { :; }
install_common_system_packages() { echo UNEXPECTED_PACKAGES; }
require_system_toolchain() { echo UNEXPECTED_TOOLCHAIN; }
prepare_build_dir() { echo UNEXPECTED_BUILD_DIR; }
run_waf_clean() { echo UNEXPECTED_WAF; }
'''
    result = shell(DEFINITIONS + stubs + MAIN, '--configure-only')
    assert result.returncode != 0
    assert 'SDK_SELECTED' in result.stdout and 'SDK_REJECTED' in result.stdout
    assert 'UNEXPECTED_' not in result.stdout
    assert 'no dependency build or fallback' in result.stderr


def test_force_dependencies_rebuilds_even_when_detector_accepts_package():
    result = shell(DEFINITIONS + '''
FORCE_DEPENDENCIES=1
is_pkg_installed() { return 0; }
ensure_source_tree() { echo SOURCE_REQUIRED >&2; return 23; }
build_waf_dependency ndn-svs libndn-svs unused 0.1
''')
    assert result.returncode == 23
    assert 'SOURCE_REQUIRED' in result.stderr


def test_user_install_rejected_before_mutations():
    result = shell(SCRIPT.read_text(), '--user', '--plan')
    assert result.returncode == 2
    assert 'incompatible' in result.stderr


def test_bootstrap_installs_os_before_toolchain_check():
    stubs = '''
    require_host_profile() { echo PROFILE; }
    install_common_system_packages() { echo PACKAGES; }
    require_system_toolchain() { echo TOOLS; }
    require_global_external_closure() { return 1; }
    source_field() { echo https://example.invalid/repo.git; }
install_external_dependencies() { echo SOURCES; }
'''
    result = shell(DEFINITIONS + stubs + MAIN, '--deps-only')
    assert result.returncode == 0, result.stderr
    assert result.stdout.index('PROFILE') < result.stdout.index('PACKAGES')
    assert result.stdout.index('PACKAGES') < result.stdout.index('TOOLS')
    assert result.stdout.index('TOOLS') < result.stdout.index('SOURCES')


def test_check_only_inventory_failure_never_installs():
    stubs = '''
require_host_profile() { :; }
require_system_toolchain() { :; }
source_field() { echo https://example.invalid/repo.git; }
install_common_system_packages() { echo UNEXPECTED; }
check_waf_inventory() { echo MISSING_GTK >&2; return 37; }
'''
    result = shell(DEFINITIONS + stubs + MAIN, '--check-dependencies')
    assert result.returncode == 37
    assert 'MISSING_GTK' in result.stderr
    assert 'UNEXPECTED' not in result.stdout


@pytest.mark.parametrize("with_onnx_adapter", [False, True])
def test_pip_builds_wheels_then_privileged_noneditable_install(with_onnx_adapter):
    package = ('"$ROOT/NDNSF-DistributedInference/packaging/python/adapters/onnx"'
               if with_onnx_adapter else '/example/package')
    result = shell(DEFINITIONS + '''
NAC_SDK_PREFIX=/usr/local/ndnsf-sdk/nac-abe/fixture
NDNSF_GLOBAL_NATIVE_DIGESTS='{"fixture":"digest"}'
stage_python_wheel_sources() { shift; PYTHON_WHEEL_INPUTS=("$@"); }
run() { printf 'BUILD:%s\\n' "$*"; touch "$wheel_dir/test.whl"; }
sudo_run() { printf 'INSTALL:%s\\n' "$*"; }
manifest_tool() { printf 'MANIFEST:%s\n' "$*"; }
pip_install ''' + package + '\n')
    assert result.returncode == 0, result.stderr
    assert '--isolated wheel --wheel-dir' in result.stdout
    assert 'NDNSF_NAC_ABE_PREFIX=/usr/local/ndnsf-sdk/nac-abe/fixture' in result.stdout
    assert 'NDNSF_GLOBAL_NATIVE_DIGESTS={"fixture":"digest"}' in result.stdout
    install = next(shlex.split(line[len('INSTALL:'):])
                   for line in result.stdout.splitlines()
                   if line.startswith('INSTALL:') and '-m pip --isolated install ' in line)
    assert {'--isolated', '--no-index', '--no-deps', '--force-reinstall'} <= set(install)
    assert not {'--user', '--editable', '-e'} & set(install)
    assert any(token.endswith('/test.whl') for token in install)
    assert result.stdout.index('BUILD:') < result.stdout.index('INSTALL:')
    manifests = [line for line in result.stdout.splitlines() if line.startswith('MANIFEST:')]
    assert len(manifests) == 2
    assert all(('ndnsf-di-adapter-onnx' in line.split()) == with_onnx_adapter for line in manifests)


def test_pip_build_receives_native_identity_in_clean_child_environment(tmp_path):
    python = tmp_path / "wheel-builder"
    python.write_text('''#!/usr/bin/python3
import os, pathlib, sys
if sys.argv[-2:] == ['help', 'install']:
    print('--break-system-packages')
    sys.exit(0)
assert os.environ['NDNSF_GLOBAL_NATIVE_DIGESTS'] == '{"fixture":"exact-native-identity"}'
assert os.environ['NDNSF_RUNTIME_PREFIX'] == '/opt/ndnsf-fixture'
assert os.environ['NDNSF_NAC_ABE_PREFIX'] == '/usr/local/ndnsf-sdk/nac-abe/fixture'
assert 'NDNSF_TEST_UNFORWARDED' not in os.environ
pathlib.Path(sys.argv[sys.argv.index('--wheel-dir') + 1], 'test.whl').touch()
print('CLEAN_WHEEL_NATIVE_IDENTITY_OK')
''')
    python.chmod(0o755)
    code = DEFINITIONS + '\nPYTHON_BIN=' + shlex.quote(str(python)) + '''
NAC_SDK_PREFIX=/usr/local/ndnsf-sdk/nac-abe/fixture
export NDNSF_GLOBAL_NATIVE_DIGESTS='{"fixture":"exact-native-identity"}'
export NDNSF_TEST_UNFORWARDED=must-not-leak
stage_python_wheel_sources() { shift; PYTHON_WHEEL_INPUTS=("$@"); }
run_bounded() { /usr/bin/env -i PATH=/usr/bin:/bin "$@"; }
manifest_tool() { :; }
sudo_run() { :; }
pip_install /example/package
'''
    result = shell(code, '--prefix', '/opt/ndnsf-fixture')
    assert result.returncode == 0, result.stderr
    assert 'CLEAN_WHEEL_NATIVE_IDENTITY_OK' in result.stdout


@pytest.mark.parametrize("modern", [False, True])
def test_installer_scopes_old_pip_bootstrap_to_its_stage(tmp_path, modern):
    python = tmp_path / "python"
    pip_help = "--break-system-packages" if modern else "old pip help"
    python.write_text("#!/bin/sh\nprintf '%s\\n' " + shlex.quote(pip_help) + "\n")
    python.chmod(0o755)
    stage = tmp_path / "private-pip"
    result = shell(DEFINITIONS + "\nPYTHON_BIN=" + shlex.quote(str(python)) + "\n" + '''
run() { printf 'BOOTSTRAP:%s\\n' "$*"; }
prepare_installer_pip ''' + shlex.quote(str(stage)) + '''
printf 'PIP_ENV:%s\\n' "${pip_bootstrap_env[*]}"
''')
    assert result.returncode == 0, result.stderr
    if modern:
        assert "BOOTSTRAP:" not in result.stdout
        assert "PIP_ENV:\n" in result.stdout
    else:
        assert f"--target {stage} pip==25.0.1" in result.stdout
        assert f"PIP_ENV:PYTHONPATH={stage}" in result.stdout


def test_native_only_stops_before_public_python_wheels(tmp_path):
    source = SCRIPT.read_text()
    start = source.index('if (( NATIVE_ONLY )); then',
                         source.index('install_native_test_python_support\n\n'))
    end = source.index('echo "==> Installing ndnsf Python wrapper"', start)
    code = DEFINITIONS + '''
release_default_sdk_scratch() { echo NATIVE_SCRATCH_RELEASED; }
''' + source[start:end] + '\necho PUBLIC_WHEELS_WOULD_START\n'
    result = shell(code, '--native-only', '--prefix', str(tmp_path / 'candidate'))
    assert result.returncode == 0, result.stderr
    assert 'Native-only candidate installed' in result.stdout
    assert 'NATIVE_SCRATCH_RELEASED' in result.stdout
    assert 'PUBLIC_WHEELS_WOULD_START' not in result.stdout


def test_non_global_native_prefix_does_not_refresh_global_linker_cache(tmp_path):
    code = DEFINITIONS + '''
PYTHON_BIN=/bin/true
install_manifest_tree() { :; }
sudo_run() { echo GLOBAL_LINKER_REFRESH; }
promote_native_stage
'''
    isolated = shell(code, '--prefix', str(tmp_path / 'candidate'))
    assert isolated.returncode == 0, isolated.stderr
    assert 'GLOBAL_LINKER_REFRESH' not in isolated.stdout
    normal = shell(code)
    assert normal.returncode == 0, normal.stderr
    assert 'GLOBAL_LINKER_REFRESH' in normal.stdout


def test_host_profile_normalizes_symlink_venv_interpreter(tmp_path):
    directory = tmp_path / 'venv/bin'
    directory.mkdir(parents=True)
    interpreter = directory / 'python3'
    interpreter.symlink_to('/usr/bin/python3')
    result = shell(DEFINITIONS + '''
uname() { echo x86_64; }
require_host_profile
printf 'PYTHON:%s\\n' "$PYTHON_BIN"
''', '--python', str(interpreter))
    assert result.returncode == 0, result.stderr
    assert 'PYTHON:/usr/bin/python3' in result.stdout


@pytest.mark.parametrize("case, message", [
    ("dependencies", None),
    ("full", None),
    ("interrupted", None),
    ("foreign-root", "not recorded as owned by this checkout"),
    ("symlink", "build directory may not be a symlink"),
    ("symlink-slash", "build directory may not be a symlink"),
    ("dangling-symlink", "build directory may not be a symlink"),
])
def test_owned_dependencies_only_build_root_reuse(tmp_path, case, message):
    build = tmp_path / "build"
    env = dict(os.environ, NDNSF_BUILD_DIR=str(build), NDNSF_REUSE_BUILD_DIR="0",
               NDNSF_TEST_PYTHON=sys.executable)
    prepare = '\nPYTHON_BIN="$NDNSF_TEST_PYTHON"\nDEPS_ONLY=1\nprepare_build_dir\n'
    first = shell(DEFINITIONS + prepare, env=env)
    assert first.returncode == 0, first.stderr
    marker = build / ".ndnsf-stack-build.json"
    original_marker = marker.read_bytes()
    assert not (build / "c4che/_cache.py").exists()
    env["NDNSF_REUSE_BUILD_DIR"] = "1"
    if case in {"full", "interrupted"}:
        prepare = prepare.replace("DEPS_ONLY=1", "DEPS_ONLY=0")
        if case == "interrupted":
            (build / "c4che").mkdir()
            (build / "c4che/_cache.py").write_text("# interrupted configuration\n")
            (build / "partial.o").write_bytes(b"")
    elif case == "foreign-root":
        foreign = tmp_path / "foreign"
        foreign.mkdir()
        env["NDNSF_TEST_OTHER_ROOT"] = str(foreign)
        prepare = '\nROOT="$NDNSF_TEST_OTHER_ROOT"\n' + prepare
    elif case in {"symlink", "symlink-slash", "dangling-symlink"}:
        alias = tmp_path / "alias"
        alias.symlink_to(tmp_path / "missing" if case == "dangling-symlink" else build,
                         target_is_directory=True)
        env["NDNSF_BUILD_DIR"] = str(alias) + ("///" if case == "symlink-slash" else "")
    result = shell(DEFINITIONS + prepare, env=env)
    if message is None:
        assert result.returncode == 0, result.stderr
        assert "Verified reusable NDNSF build directory" in result.stdout
    else:
        assert result.returncode != 0
        assert message in result.stderr
    assert marker.read_bytes() == original_marker
    assert (build / "c4che/_cache.py").exists() == (case == "interrupted")
    if case == "interrupted":
        assert (build / "partial.o").read_bytes() == b""


@pytest.mark.parametrize('receipt_matches', [False, True])
def test_openabe_compatible_abi_requires_locked_source_receipt(tmp_path, receipt_matches):
    helper = tmp_path / 'source-helper.sh'
    helper.write_text('printf "%s\\n" "$*" >> "$RECEIPT_CALLS"\nexit ' +
                      ('0' if receipt_matches else '1') + '\n')
    calls = tmp_path / 'calls'
    code = DEFINITIONS + '\n' + 'SOURCE_HELPER=' + shlex.quote(str(helper)) + '\n'
    code += "PYTHON_BIN=/bin/bash\nhas_openabe() { return 0; }\n"
    code += "ensure_source_tree() { echo SOURCE_REQUIRED >&2; return 23; }\n"
    code += "build_openabe_dependency\n"
    result = shell(code, env=dict(os.environ, RECEIPT_CALLS=str(calls)))
    assert result.returncode == (0 if receipt_matches else 23), result.stderr
    assert ('Locked patched OpenABE already installed' in result.stdout) == receipt_matches
    assert ('SOURCE_REQUIRED' in result.stderr) != receipt_matches
    invocation = calls.read_text()
    assert 'check --lock ' in invocation
    assert '--name openabe --prefix /usr/local' in invocation


def test_openabe_materialization_failure_stops_before_build(tmp_path):
    helper = tmp_path / 'source-helper.sh'
    helper.write_text('printf "%s\\n" "$*" >> "$MATERIALIZE_CALLS"\nexit 29\n')
    calls = tmp_path / 'calls'
    code = DEFINITIONS + '\n' + 'SOURCE_HELPER=' + shlex.quote(str(helper)) + '\n'
    code += 'PYTHON_BIN=/bin/bash\nhas_openabe() { return 1; }\n'
    code += 'ensure_source_tree() { echo /fixture/clean-openabe; }\n'
    code += 'run() { echo UNEXPECTED_BUILD >&2; return 99; }\n'
    code += 'build_openabe_dependency\n'
    result = shell(code, env=dict(os.environ, MATERIALIZE_CALLS=str(calls)))
    assert result.returncode == 1, result.stderr
    assert 'UNEXPECTED_BUILD' not in result.stderr
    invocation = calls.read_text()
    assert 'materialize --lock ' in invocation
    assert '--name openabe --source-dir /fixture/clean-openabe --build-root ' in invocation
